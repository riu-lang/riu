// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#undef ERROR
#endif

#include "workspace.h"

#include "ast/ast_builder.h"
#include "ast/node/file_node.h"
#include "ast/yux.h"

#include "antlr4-runtime.h"
#include "yux/yuxLexer.h"
#include "yux/yuxParser.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <stdexcept>

namespace fs = std::filesystem;
using namespace yux;

namespace yux::lsp {

namespace {

int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string urlDecode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            int hi = hexVal(s[i + 1]);
            int lo = hexVal(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        if (s[i] == '+')
            out.push_back(' ');
        else
            out.push_back(s[i]);
    }
    return out;
}

std::string urlEncodePath(const std::string& s) {
    // 只对保留字符做最小转义（: 在 file URI scheme 部分不需要转，
    // 但在路径里 vscode 习惯转。这里保守只转空格与控制字符）
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (c <= 0x20 || c == '#' || c == '?') {
            std::array<char, 4> buf{};
            std::snprintf(buf.data(), buf.size(), "%%%02X", c);
            out.append(buf.data());
        } else {
            out.push_back(static_cast<char>(c));
        }
    }
    return out;
}

// 取当前可执行文件路径（仅 Windows 实现；非 Windows 返回空，依赖前两条兜底）
std::string getMainExecutablePath() {
#ifdef _WIN32
    std::array<wchar_t, MAX_PATH> buf{};
    DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0 || n >= MAX_PATH) return {};
    return fs::path(std::wstring(buf.data(), n)).string();
#else
    return {};
#endif
}

std::string findSdkPathForLsp() {
    // 1. 显式 env 覆盖（用于 LSP 部署 / 测试）
    if (const char* env = std::getenv("YUX_SDK_PATH")) {
        if (fs::is_directory(env)) return env;
    }
    // 2. 仓库内调试：cwd
    if (fs::is_directory("sdk/yux/core")) return "sdk/yux/core";
    // 3. exe 旁的 ../sdk/yux/core
    std::string exe = getMainExecutablePath();
    if (!exe.empty()) {
        fs::path exePath(exe);
        fs::path sdk = exePath.parent_path().parent_path() / "sdk" / "yux" / "core";
        if (fs::is_directory(sdk)) return sdk.string();
    }
    return "";
}

// 解析 SDK 目录的所有 .yux 文件到 yux 实例。失败抛 runtime_error。
void parseSdkInto(const std::string& sdkDir, Yux& yux) {
    std::vector<std::string> files;
    for (const auto& e : fs::directory_iterator(sdkDir)) {
        if (!e.is_regular_file()) continue;
        auto p = e.path().string();
        if (p.size() > 4 && p.substr(p.size() - 4) == ".yux") files.push_back(p);
    }
    std::ranges::sort(files);
    for (const auto& f : files) {
        antlr4::ANTLRFileStream stream;
        stream.loadFromFile(f);
        ::yux::yuxLexer lexer(&stream);
        antlr4::CommonTokenStream tokens(&lexer);
        ::yux::yuxParser parser(&tokens);
        auto program = parser.program();
        if (parser.getNumberOfSyntaxErrors()) {
            throw std::runtime_error("SDK syntax error: " + f);
        }
        ASTBuilder ab(yux, "yux.core", true);
        ab.build(program); // 抛 YuxError 自然向上传
    }
}

} // namespace

std::string uriToPath(const std::string& uri) {
    constexpr const char* kPrefix = "file://";
    if (!uri.starts_with(kPrefix)) return {};
    std::string body = uri.substr(std::strlen(kPrefix));
    body = urlDecode(body);
#ifdef _WIN32
    // file:///C:/foo → /C:/foo → C:/foo
    if (body.size() >= 3 && body[0] == '/' && body[2] == ':') {
        body.erase(0, 1);
    }
#endif
    return normalizePath(body);
}

std::string pathToUri(const std::string& absPath) {
    std::string norm = normalizePath(absPath);
#ifdef _WIN32
    return "file:///" + urlEncodePath(norm);
#else
    if (!norm.empty() && norm[0] == '/') return "file://" + urlEncodePath(norm);
    return "file:///" + urlEncodePath(norm);
#endif
}

std::string normalizePath(const std::string& p) {
    std::string s = p;
    for (auto& c : s)
        if (c == '\\') c = '/';
#ifdef _WIN32
    if (s.size() >= 2 && s[1] == ':') {
        s[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(s[0])));
    }
#endif
    // 清理 ./ 与 ../
    try {
        auto canon = fs::weakly_canonical(s).string();
        for (auto& c : canon)
            if (c == '\\') c = '/';
#ifdef _WIN32
        if (canon.size() >= 2 && canon[1] == ':') {
            canon[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(canon[0])));
        }
#endif
        return canon;
    } catch (...) {
        return s;
    }
}

// =========================== Project ===========================

Project::Project() = default;
Project::~Project() = default;

bool Project::buildProject(const std::string& rootDir) {
    _mode = Mode::Project;
    _rootDir = rootDir;
    _rootKey = normalizePath(rootDir);
    return rebuild();
}

bool Project::buildSingleFile(const std::string& mainAbsPath) {
    _mode = Mode::SingleFile;
    _mainPath = normalizePath(mainAbsPath);
    _rootKey = _mainPath;
    return rebuild();
}

bool Project::rebuild() {
    _ok = false;
    _buildError.clear();
    _mainFile = nullptr;
    _byPath.clear();
    _yux = std::make_unique<Yux>();

    try {
        if (_mode == Mode::Project) {
            _yux->initProjectFromDir(_rootDir);
            const auto& entry = _yux->projectEntry();
            if (entry.empty()) {
                _buildError = "yux.toml missing `entry`";
                return false;
            }
            std::string entryPath = (fs::path(_yux->projectRoot()) / entry).string();
            if (!fs::exists(entryPath)) {
                _buildError = "entry file not found: " + entryPath;
                return false;
            }
            _mainPath = normalizePath(fs::absolute(entryPath).string());
        } else {
            _yux->initFileRoot(_mainPath);
        }

        std::string sdkDir = findSdkPathForLsp();
        if (!sdkDir.empty()) {
            try {
                parseSdkInto(sdkDir, *_yux);
            } catch (const std::exception& e) {
                // SDK 失败不致命：仅记录，仍然加载主文件
                _buildError = std::string("SDK parse failed: ") + e.what();
            }
        }

        std::string baseName = fs::path(_mainPath).stem().string();
        _mainFile = _yux->loadMainFile(_mainPath, baseName);
    } catch (const std::exception& e) {
        if (_buildError.empty())
            _buildError = e.what();
        else
            _buildError += "; " + std::string(e.what());
        return false;
    }

    rebuildIndex();
    _ok = (_mainFile != nullptr);
    return _ok;
}

void Project::rebuildIndex() {
    _byPath.clear();
    if (_mainFile) {
        _byPath[_mainPath] = _mainFile;
    }
    if (!_yux) return;
    for (const auto& mod : _yux->loadOrder()) {
        std::string path = _yux->modulePath(mod);
        if (path.empty()) continue;
        std::string key = normalizePath(path);
        FileNode* fn = _yux->module(mod);
        if (fn) _byPath[key] = fn;
    }
}

FileNode* Project::fileForPath(const std::string& normPath) const {
    auto it = _byPath.find(normPath);
    return it != _byPath.end() ? it->second : nullptr;
}

// =========================== Workspace ===========================

std::string Workspace::findProjectRoot(const std::string& docAbsPath) {
    fs::path p(docAbsPath);
    if (fs::is_regular_file(p)) p = p.parent_path();
    while (!p.empty()) {
        if (fs::exists(p / "yux.toml")) return p.string();
        auto parent = p.parent_path();
        if (parent == p) break;
        p = parent;
    }
    return {};
}

Project* Workspace::projectForUri(const std::string& uri) {
    std::string path = uriToPath(uri);
    if (path.empty()) return nullptr;
    std::string root = findProjectRoot(path);
    std::string key;
    if (!root.empty()) {
        key = normalizePath(root);
    } else {
        key = path; // 单文件模式 key = 文件本身
    }
    auto it = _projects.find(key);
    if (it != _projects.end()) return it->second.get();
    auto proj = std::make_unique<Project>();
    if (!root.empty()) {
        proj->buildProject(root);
    } else {
        proj->buildSingleFile(path);
    }
    Project* raw = proj.get();
    _projects[key] = std::move(proj);
    return raw;
}

Project* Workspace::rebuildForUri(const std::string& uri) {
    std::string path = uriToPath(uri);
    if (path.empty()) return nullptr;
    std::string root = findProjectRoot(path);
    std::string key = root.empty() ? path : normalizePath(root);
    auto it = _projects.find(key);
    if (it == _projects.end()) return projectForUri(uri); // 还没构建过则首次构建
    it->second->rebuild();
    return it->second.get();
}

Workspace::Resolved Workspace::resolveUri(const std::string& uri) {
    Resolved r;
    r.normPath = uriToPath(uri);
    r.project = projectForUri(uri);
    if (r.project && !r.normPath.empty()) {
        r.file = r.project->fileForPath(r.normPath);
    }
    return r;
}

} // namespace yux::lsp
