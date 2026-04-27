// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// LSP 传输层实现
//
// 本文件实现按 LSP 规范读写 stdio JSON-RPC 消息:
// - 头部仅识别 Content-Length，其它头（Content-Type 等）忽略
// - 头部以 \r\n 分隔，头部块结束于空行 \r\n\r\n
// - body 按 Content-Length 字节数原样读取，不做任何编码转换

#include "lsp_io.h"

#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace yux::lsp {

void setStdioBinary() {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
}

// 从 stdin 读一行（以 \r\n 结尾），返回不含 \r\n 的内容。
// EOF 返回 std::nullopt。
static std::optional<std::string> readHeaderLine() {
    std::string line;
    while (true) {
        int c = std::fgetc(stdin);
        if (c == EOF) {
            return std::nullopt;
        }
        if (c == '\r') {
            int n = std::fgetc(stdin);
            if (n == '\n') {
                return line;
            }
            // 协议异常：\r 后必须是 \n。容错：把 \r 当普通字符塞回去
            line.push_back(static_cast<char>(c));
            if (n != EOF) {
                line.push_back(static_cast<char>(n));
            }
            continue;
        }
        line.push_back(static_cast<char>(c));
    }
}

std::optional<std::string> readMessage() {
    long long contentLength = -1;
    while (true) {
        auto line = readHeaderLine();
        if (!line) return std::nullopt;
        if (line->empty()) break; // 头部结束
        // 解析 Content-Length: N
        const std::string key = "Content-Length:";
        if (line->starts_with(key)) {
            const char* p = line->c_str() + key.size();
            while (*p == ' ' || *p == '\t') ++p;
            contentLength = std::strtoll(p, nullptr, 10);
        }
        // 其它头一律忽略
    }
    if (contentLength < 0) {
        // 无 Content-Length：协议错误，跳过本条
        return std::string{};
    }
    std::string body;
    body.resize(static_cast<size_t>(contentLength));
    size_t got = std::fread(body.data(), 1, body.size(), stdin);
    if (got != body.size()) {
        return std::nullopt;
    }
    return body;
}

void writeMessage(const std::string& body) {
    // 头部和 body 一次性写出，避免被外部观察到半截消息
    std::string out;
    out.reserve(body.size() + 32);
    out += "Content-Length: ";
    out += std::to_string(body.size());
    out += "\r\n\r\n";
    out += body;
    std::fwrite(out.data(), 1, out.size(), stdout);
    std::fflush(stdout);
}

} // namespace yux::lsp
