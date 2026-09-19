// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// LSP 服务主循环实现
//
// 本文件实现 LSP server 的请求分发与状态机:
// - 主循环：readMessage -> 解析 JSON -> 按 method 派发 -> 写回响应
// - 握手状态机：仅在 initialize 完成后才接受其它请求；shutdown 后只接受 exit
// - 文档同步：textDocument/didOpen|didChange|didClose 维护 uri -> 文本
// - P0 阶段：能力声明只开 textDocumentSync.full，其它 provider 留给 P1+
// - 错误处理：JSON 解析失败回 -32700；未知请求回 -32601；未初始化回 -32002

#include "lsp_server.h"
#include "ast/node/file_node.h"
#include "ast/node/fn_node.h"
#include "ast/node/global_const_node.h"
#include "ast/node/struct_node.h"
#include "completion.h"
#include "document.h"
#include "lsp_io.h"
#include "position.h"
#include "semantic_tokens.h"
#include "symbol_lookup.h"
#include "tools/format/printer.h"
#include "utf8.h"
#include "workspace.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <iostream>
#include <optional>
#include <string>

namespace riu::lsp {

using json = nlohmann::json;

namespace {

struct ServerState {
    bool initialized = false;  // 收到 initialized 通知
    bool shuttingDown = false; // 收到 shutdown 请求
    DocumentManager docs;
    Workspace ws;
};

// JSON-RPC 错误码（节选自 LSP 规范）
constexpr int kErrParseError = -32700;
constexpr int kErrInvalidRequest = -32600;
constexpr int kErrMethodNotFound = -32601;
constexpr int kErrServerNotInitialized = -32002;

// 把任意 id 字段（可能是 number / string / null / 缺失）原样取出
static json extractId(const json& msg) {
    auto it = msg.find("id");
    if (it == msg.end()) return nullptr;
    return *it;
}

static void sendResult(const json& id, json result) {
    json msg = {
        {"jsonrpc", "2.0"},
        {"id", id},
        {"result", std::move(result)},
    };
    writeMessage(msg.dump());
}

static void sendError(const json& id, int code, const std::string& message) {
    json msg = {
        {"jsonrpc", "2.0"},
        {"id", id},
        {"error", {{"code", code}, {"message", message}}},
    };
    writeMessage(msg.dump());
}

// 构造 initialize 响应中的 capabilities
static json buildCapabilities() {
    return {
        // textDocumentSync: 1 = Full（每次 didChange 推送完整文本）
        {"textDocumentSync",
         {
             {"openClose", true},
             {"change", 1},
             // P2 Plan A：保存时重建 Project 索引
             {"save", json::object({{"includeText", false}})},
         }},
        {"documentSymbolProvider", true},
        {"documentFormattingProvider", true},
        {"completionProvider",
         {
             {"triggerCharacters", json::array({" ", ".", "(", "{", "["})},
         }},
        {"definitionProvider", true},
        {"hoverProvider", true},
        {"signatureHelpProvider",
         {
             {"triggerCharacters", json::array({"(", ","})},
         }},
        {"semanticTokensProvider",
         {
             {"legend",
              {
                  {"tokenTypes", json(semanticTokenTypes())},
                  {"tokenModifiers", json(semanticTokenModifiers())},
              }},
             {"full", true},
         }},
    };
}

// 把内部 LspPosition 转 JSON Position
static json posToJson(const LspPosition& p) {
    return {{"line", p.line}, {"character", p.character}};
}

static json rangeToJson(const LspPosition& s, const LspPosition& e) {
    return {{"start", posToJson(s)}, {"end", posToJson(e)}};
}

// 推送 textDocument/publishDiagnostics 通知
static void publishDiagnostics(Document& doc) {
    json diags = json::array();
    for (const auto& d : doc.diagnostics()) {
        json djson = {
            {"range", rangeToJson(d.start, d.end)},
            {"severity", static_cast<int>(d.severity)},
            {"source", "riu"},
            {"message", d.message},
        };
        if (!d.code.empty()) djson["code"] = d.code;
        diags.push_back(std::move(djson));
    }
    json msg = {
        {"jsonrpc", "2.0"},
        {"method", "textDocument/publishDiagnostics"},
        {"params",
         {
             {"uri", doc.uri()},
             {"version", doc.version()},
             {"diagnostics", std::move(diags)},
         }},
    };
    writeMessage(msg.dump());
}

// 解析并推送诊断；P1 阶段同步执行（小文件解析 < 1ms）
static void parseAndPublish(Document* doc) {
    if (!doc) return;
    doc->parseIfDirty();
    publishDiagnostics(*doc);
}

static void handleInitialize(ServerState& st, const json& msg) {
    const json id = extractId(msg);
    json result = {
        {"capabilities", buildCapabilities()},
        {"serverInfo", {{"name", "riu-lsp"}, {"version", "0.1.1"}}},
    };
    sendResult(id, std::move(result));
    (void)st;
}

static void handleShutdown(ServerState& st, const json& msg) {
    st.shuttingDown = true;
    sendResult(extractId(msg), nullptr);
}

// 安全地从 params 取 textDocument.uri
static std::string textDocumentUri(const json& params) {
    auto td = params.find("textDocument");
    if (td == params.end() || !td->is_object()) return {};
    auto uri = td->find("uri");
    if (uri == td->end() || !uri->is_string()) return {};
    return uri->get<std::string>();
}

static void handleDidOpen(ServerState& st, const json& params) {
    const std::string uri = textDocumentUri(params);
    if (uri.empty()) return;
    const auto& td = params.at("textDocument");
    int version = td.value("version", 0);
    std::string text = td.value("text", std::string{});
    Document* doc = st.docs.open(uri, version, std::move(text));
    parseAndPublish(doc);
    // 触发工作区索引（首次构建是 lazy 的）
    st.ws.projectForUri(uri);
}

static void handleDidSave(ServerState& st, const json& params) {
    const std::string uri = textDocumentUri(params);
    if (uri.empty()) return;
    // 重建该文档所属项目（Plan A：保存时重建）
    st.ws.rebuildForUri(uri);
}

static void handleDidChange(ServerState& st, const json& params) {
    const std::string uri = textDocumentUri(params);
    if (uri.empty()) return;
    Document* doc = st.docs.get(uri);
    if (!doc) return;
    const auto& td = params.at("textDocument");
    int version = td.value("version", doc->version());
    std::string text = doc->text();
    // Full 同步模式：取最后一个 contentChanges[].text
    if (auto changes = params.find("contentChanges");
        changes != params.end() && changes->is_array() && !changes->empty()) {
        const auto& last = changes->back();
        if (last.is_object()) {
            text = last.value("text", text);
        }
    }
    doc = st.docs.update(uri, version, std::move(text));
    parseAndPublish(doc);
}

static void handleDidClose(ServerState& st, const json& params) {
    const std::string uri = textDocumentUri(params);
    if (uri.empty()) return;
    st.docs.close(uri);
    // 关闭时清空诊断
    json msg = {
        {"jsonrpc", "2.0"},
        {"method", "textDocument/publishDiagnostics"},
        {"params", {{"uri", uri}, {"diagnostics", json::array()}}},
    };
    writeMessage(msg.dump());
}

// LSP SymbolKind: 我们的枚举已经是协议数值
static int symbolKindToInt(SymbolKind k) {
    return static_cast<int>(k);
}

static json symbolToJson(const DocSymbol& s) {
    return {
        {"name", s.name},
        {"kind", symbolKindToInt(s.kind)},
        {"range", rangeToJson(s.rangeStart, s.rangeEnd)},
        {"selectionRange", rangeToJson(s.selStart, s.selEnd)},
    };
}

// 计算文档末尾的 LSP Position（用于 formatting 的全文 range）
static LspPosition documentEndPos(const std::string& text) {
    LspPosition p;
    if (text.empty()) return p;
    // 行 = 换行数；列 = 最后一行的 UTF-16 code unit 数
    int line = 0;
    size_t lastLineStart = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\n') {
            ++line;
            lastLineStart = i + 1;
        }
    }
    p.line = line;
    // 末行 UTF-16 长度
    int utf16 = 0;
    auto it = text.begin() + static_cast<std::ptrdiff_t>(lastLineStart);
    auto end = text.end();
    try {
        while (it < end) {
            char32_t cp = utf8::next(it, end);
            utf16 += (cp <= 0xFFFF ? 1 : 2);
        }
    } catch (...) {
        // 残缺 UTF-8：按字节兜底
        utf16 = static_cast<int>(text.size() - lastLineStart);
    }
    p.character = utf16;
    return p;
}

static void handleFormatting(ServerState& st, const json& msg) {
    const json id = extractId(msg);
    const json params = msg.value("params", json::object());
    const std::string uri = textDocumentUri(params);
    Document* doc = uri.empty() ? nullptr : st.docs.get(uri);
    if (!doc) {
        sendResult(id, json::array());
        return;
    }
    std::string formatted;
    try {
        formatted = ::riu::format::formatAst(doc->text(), {});
    } catch (const std::exception& e) {
        sendError(id, -32603, std::string("formatter error: ") + e.what());
        return;
    }
    if (formatted == doc->text()) {
        sendResult(id, json::array());
        return;
    }
    LspPosition end = documentEndPos(doc->text());
    json edits = json::array();
    edits.push_back({
        {"range", rangeToJson({.line = 0, .character = 0}, end)},
        {"newText", std::move(formatted)},
    });
    sendResult(id, std::move(edits));
}

// LSP CompletionItemKind 协议常量（仅用到的）
constexpr int kCikFunction = 3;
constexpr int kCikConstant = 21;
constexpr int kCikStruct = 22;

// LSP CompletionItemKind: Field=5, Method=2
constexpr int kCikField = 5;
constexpr int kCikMethod = 2;

static std::optional<LspPosition> jsonToPos(const json& params);
static FnNode* findEnclosingFn(FileNode* file, int lspLine0);
static std::string resolveReceiverStructName(FnNode* fn, const std::string& name);

static void handleCompletion(ServerState& st, const json& msg) {
    const json id = extractId(msg);
    const json params = msg.value("params", json::object());
    json items = json::array();

    // receiver.member 上下文优先：仅返回结构体成员（不混静态项 / 全局名）
    {
        const std::string uri = textDocumentUri(params);
        Document* doc = uri.empty() ? nullptr : st.docs.get(uri);
        auto pos = jsonToPos(params);
        if (doc && pos) {
            auto rc = receiverContextAt(doc->text(), *pos);
            if (rc.found) {
                auto resolved = st.ws.resolveUri(uri);
                if (resolved.project && resolved.file) {
                    FnNode* fn = findEnclosingFn(resolved.file, pos->line);
                    std::string structName = resolveReceiverStructName(fn, rc.receiver);
                    if (!structName.empty()) {
                        auto mems = collectStructMembers(*resolved.project, resolved.file, structName);
                        std::set<std::string> seen;
                        for (auto* sd : mems.structs) {
                            for (const auto& f : sd->fields()) {
                                std::string nm = f->name().getText();
                                if (!seen.insert("f:" + nm).second) continue;
                                items.push_back({
                                    {"label", nm},
                                    {"kind", kCikField},
                                    {"detail", nm + " " + typeInfoDisplay(f->getType())},
                                });
                            }
                        }
                        for (auto* m : mems.methods) {
                            std::string nm = m->header()->name().getText();
                            // 重载只展示一次（detail 用首个签名）
                            if (!seen.insert("m:" + nm).second) continue;
                            auto sig = renderSignature(m);
                            items.push_back({
                                {"label", nm},
                                {"kind", kCikMethod},
                                {"detail", sig.label},
                            });
                        }
                        sendResult(id, json{
                                           {"isIncomplete", false},
                                           {"items", std::move(items)},
                                       });
                        return;
                    }
                }
                // receiver 解析失败 → 仍返回空成员列表，避免误展示全局名
                sendResult(id, json{
                                   {"isIncomplete", false},
                                   {"items", json::array()},
                               });
                return;
            }
        }
    }

    // 1. 静态项（关键字 / 类型 / 内置函数 / snippet）
    for (const auto& it : staticCompletions()) {
        json item = {
            {"label", it.label},
            {"kind", static_cast<int>(it.kind)},
        };
        if (!it.detail.empty()) item["detail"] = it.detail;
        if (!it.insertText.empty()) {
            item["insertText"] = it.insertText;
            item["insertTextFormat"] = static_cast<int>(it.format);
        }
        if (!it.documentation.empty()) item["documentation"] = it.documentation;
        items.push_back(std::move(item));
    }

    // 2. 动态项：项目内可见的 fn / struct / let
    const std::string uri = textDocumentUri(params);
    if (!uri.empty()) {
        auto resolved = st.ws.resolveUri(uri);
        if (resolved.project && resolved.file) {
            auto vis = collectVisible(*resolved.project, resolved.file);
            for (auto* fn : vis.functions) {
                auto sig = renderSignature(fn);
                items.push_back({
                    {"label", fn->header()->name().getText()},
                    {"kind", kCikFunction},
                    {"detail", sig.label},
                });
            }
            for (auto* sd : vis.structs) {
                items.push_back({
                    {"label", sd->name().getText()},
                    {"kind", kCikStruct},
                    {"detail", "struct " + sd->name().getText()},
                });
            }
            for (auto* gc : vis.consts) {
                items.push_back({
                    {"label", gc->name().getText()},
                    {"kind", kCikConstant},
                    {"detail", "cval " + gc->name().getText() + " " + typeInfoDisplay(gc->getType())},
                });
            }
        }
    }

    json result = {
        {"isIncomplete", false},
        {"items", std::move(items)},
    };
    sendResult(id, std::move(result));
}

// 把 LookupResult 中的目标名定位到具体文件 → URI + Range
// 优先使用 ownerFile；从 Workspace 反查 owner 的归一化路径（再转 URI）
static std::optional<std::pair<std::string, std::pair<LspPosition, LspPosition>>> locateLookup(Project& project,
                                                                                               const LookupResult& r) {
    if (r.kind == LookupResult::Kind::None || !r.ownerFile) return std::nullopt;
    std::string ownerPath;
    for (const auto& kv : project.allFiles()) {
        if (kv.second == r.ownerFile) {
            ownerPath = kv.first;
            break;
        }
    }
    if (ownerPath.empty()) return std::nullopt;
    Token tok;
    switch (r.kind) {
    case LookupResult::Kind::Function:
        if (!r.fn) return std::nullopt;
        tok = r.fn->header()->name();
        break;
    case LookupResult::Kind::Struct:
        if (!r.sd) return std::nullopt;
        tok = r.sd->name();
        break;
    case LookupResult::Kind::GlobalConst:
        if (!r.gc) return std::nullopt;
        tok = r.gc->name();
        break;
    default:
        return std::nullopt;
    }
    LspPosition s, e;
    tokenToLspRangeFromFile(ownerPath, tok, s, e);
    return std::make_pair(pathToUri(ownerPath), std::make_pair(s, e));
}

// 根据光标行号在文件中找到包含它的最近 fn（含 struct impl 方法）。
// 仅按起始行筛选 + 取最大者；无 end-line 信息，对单文件场景已足够。
static FnNode* findEnclosingFn(FileNode* file, int lspLine0) {
    if (!file) return nullptr;
    int targetLine = lspLine0 + 1; // 节点行号 1-based
    FnNode* best = nullptr;
    int bestLine = 0;
    auto consider = [&](FnNode* fn) {
        if (!fn) return;
        int ln = fn->getLineNumber();
        if (ln <= targetLine && ln > bestLine) {
            best = fn;
            bestLine = ln;
        }
    };
    for (const auto& fn : file->getFunctions())
        consider(fn);
    for (const auto& si : file->getStructImpls()) {
        for (const auto& m : si->methods())
            consider(m);
    }
    return best;
}

// 从 receiver 名解析其结构体类型名（含 "Rc<T>"/"T?" 解包到内部 T；
// 若 T 是结构体则返回 T 名）。失败返回空串。
static std::string resolveReceiverStructName(FnNode* fn, const std::string& name) {
    if (!fn) return {};
    auto* sym = fn->lookupSymbol(name);
    if (!sym) return {};
    TypeInfo t = *sym->type;
    // 解包常见包装：Rc<T>/Nullable<T>/Ref —— 仅对成员补全/跳转一层展开
    if (t.isRef()) {
        if (auto e = t.refElementType()) t = *e;
    }
    if (t.isRc()) {
        if (auto e = t.rcElementType()) t = *e;
    }
    return t.name;
}

static std::optional<LspPosition> jsonToPos(const json& params) {
    auto it = params.find("position");
    if (it == params.end() || !it->is_object()) return std::nullopt;
    LspPosition p;
    p.line = it->value("line", 0);
    p.character = it->value("character", 0);
    return p;
}

// 把 (token, ownerFile) 转 LSP Location 链接 JSON
static std::optional<json> tokenLocLink(Project& project, FileNode* ownerFile, const Token& tok,
                                        const LspPosition& origStart, const LspPosition& origEnd) {
    std::string ownerPath;
    for (const auto& kv : project.allFiles()) {
        if (kv.second == ownerFile) {
            ownerPath = kv.first;
            break;
        }
    }
    if (ownerPath.empty()) return std::nullopt;
    LspPosition s, e;
    tokenToLspRangeFromFile(ownerPath, tok, s, e);
    return json{
        {"originSelectionRange", rangeToJson(origStart, origEnd)},
        {"targetUri", pathToUri(ownerPath)},
        {"targetRange", rangeToJson(s, e)},
        {"targetSelectionRange", rangeToJson(s, e)},
    };
}

static void handleDefinition(ServerState& st, const json& msg) {
    const json id = extractId(msg);
    const json params = msg.value("params", json::object());
    const std::string uri = textDocumentUri(params);
    Document* doc = uri.empty() ? nullptr : st.docs.get(uri);
    auto pos = jsonToPos(params);
    if (!doc || !pos) {
        sendResult(id, nullptr);
        return;
    }
    auto hit = identifierAt(doc->text(), *pos);
    if (!hit.found) {
        sendResult(id, nullptr);
        return;
    }
    auto resolved = st.ws.resolveUri(uri);
    if (!resolved.project || !resolved.file) {
        sendResult(id, nullptr);
        return;
    }

    // receiver.member 优先：仅当 hit IDENT 紧邻在 `IDENT . ` 之后才走成员路径
    auto rc = receiverContextAt(doc->text(), *pos);
    if (rc.found && !rc.member.empty() && rc.member == hit.text) {
        FnNode* fn = findEnclosingFn(resolved.file, pos->line);
        std::string structName = resolveReceiverStructName(fn, rc.receiver);
        if (!structName.empty()) {
            auto mh = lookupStructMember(*resolved.project, resolved.file, structName, rc.member);
            std::optional<json> link;
            if (mh.kind == MemberHit::Kind::Field && mh.field) {
                link = tokenLocLink(*resolved.project, mh.ownerFile, mh.field->name(), hit.start, hit.end);
            } else if (mh.kind == MemberHit::Kind::Method && mh.method) {
                link = tokenLocLink(*resolved.project, mh.ownerFile, mh.method->header()->name(), hit.start, hit.end);
            }
            if (link) {
                sendResult(id, json::array({*link}));
                return;
            }
            // 成员未找到 → 不再回退到全局，避免误跳
            sendResult(id, nullptr);
            return;
        }
        // 类型解析失败（receiver 不在当前 fn 里） → 回退全局
    }

    auto lk = lookupName(*resolved.project, resolved.file, hit.text);
    auto loc = locateLookup(*resolved.project, lk);
    if (!loc) {
        sendResult(id, nullptr);
        return;
    }
    // 始终返回 LocationLink：originSelectionRange 限制 ctrl-hover 时
    // 客户端显示的可点击下划线范围（否则没有 PSI 的客户端会按整行兜底）。
    // LSP4IJ 等现代客户端均支持 LocationLink；不支持的客户端会忽略。
    json link = {
        {"originSelectionRange", rangeToJson(hit.start, hit.end)},
        {"targetUri", loc->first},
        {"targetRange", rangeToJson(loc->second.first, loc->second.second)},
        {"targetSelectionRange", rangeToJson(loc->second.first, loc->second.second)},
    };
    sendResult(id, json::array({std::move(link)}));
}

static void handleHover(ServerState& st, const json& msg) {
    const json id = extractId(msg);
    const json params = msg.value("params", json::object());
    const std::string uri = textDocumentUri(params);
    Document* doc = uri.empty() ? nullptr : st.docs.get(uri);
    auto pos = jsonToPos(params);
    if (!doc || !pos) {
        sendResult(id, nullptr);
        return;
    }
    auto hit = identifierAt(doc->text(), *pos);
    if (!hit.found) {
        sendResult(id, nullptr);
        return;
    }
    auto resolved = st.ws.resolveUri(uri);
    if (!resolved.project || !resolved.file) {
        sendResult(id, nullptr);
        return;
    }
    auto lk = lookupName(*resolved.project, resolved.file, hit.text);
    std::string md;
    switch (lk.kind) {
    case LookupResult::Kind::Function:
        md = renderFnHover(lk.fn);
        break;
    case LookupResult::Kind::Struct:
        md = renderStructHover(lk.sd);
        break;
    case LookupResult::Kind::GlobalConst:
        md = renderConstHover(lk.gc);
        break;
    default:
        break;
    }
    if (md.empty()) {
        sendResult(id, nullptr);
        return;
    }
    json result = {
        {"contents", {{"kind", "markdown"}, {"value", md}}},
        {"range", rangeToJson(hit.start, hit.end)},
    };
    sendResult(id, std::move(result));
}

static void handleSignatureHelp(ServerState& st, const json& msg) {
    const json id = extractId(msg);
    const json params = msg.value("params", json::object());
    const std::string uri = textDocumentUri(params);
    Document* doc = uri.empty() ? nullptr : st.docs.get(uri);
    auto pos = jsonToPos(params);
    if (!doc || !pos) {
        sendResult(id, nullptr);
        return;
    }
    auto cc = findEnclosingCall(doc->text(), *pos);
    if (!cc.found) {
        sendResult(id, nullptr);
        return;
    }
    auto resolved = st.ws.resolveUri(uri);
    if (!resolved.project || !resolved.file) {
        sendResult(id, nullptr);
        return;
    }
    auto lk = lookupName(*resolved.project, resolved.file, cc.callee);
    if (lk.kind != LookupResult::Kind::Function || lk.overloads.empty()) {
        sendResult(id, nullptr);
        return;
    }
    json sigs = json::array();
    for (auto* fn : lk.overloads) {
        auto sig = renderSignature(fn);
        json params_arr = json::array();
        for (const auto& pr : sig.params) {
            params_arr.push_back({{"label", json::array({pr.first, pr.second})}});
        }
        sigs.push_back({
            {"label", sig.label},
            {"parameters", std::move(params_arr)},
        });
    }
    json result = {
        {"signatures", std::move(sigs)},
        {"activeSignature", 0},
        {"activeParameter", cc.activeParam},
    };
    sendResult(id, std::move(result));
}

static void handleSemanticTokensFull(ServerState& st, const json& msg) {
    const json id = extractId(msg);
    const json params = msg.value("params", json::object());
    const std::string uri = textDocumentUri(params);
    Document* doc = uri.empty() ? nullptr : st.docs.get(uri);
    json data = json::array();
    if (doc) {
        auto encoded = computeSemanticTokens(doc->text());
        for (int v : encoded)
            data.push_back(v);
    }
    sendResult(id, json{{"data", std::move(data)}});
}

static void handleDocumentSymbol(ServerState& st, const json& msg) {
    const json id = extractId(msg);
    const json params = msg.value("params", json::object());
    const std::string uri = textDocumentUri(params);
    Document* doc = uri.empty() ? nullptr : st.docs.get(uri);
    if (!doc) {
        sendResult(id, json::array());
        return;
    }
    doc->parseIfDirty();
    json arr = json::array();
    for (const auto& s : doc->symbols())
        arr.push_back(symbolToJson(s));
    sendResult(id, std::move(arr));
}

// 分发：返回 false 表示需要退出主循环
static bool dispatch(ServerState& st, const json& msg) {
    const auto methodIt = msg.find("method");
    const bool isRequest = msg.contains("id");
    const std::string method =
        (methodIt != msg.end() && methodIt->is_string()) ? methodIt->get<std::string>() : std::string{};

    // exit 通知：必须立即退出
    if (method == "exit") {
        return false;
    }

    // shutdown 之后，除 exit 外一律拒绝
    if (st.shuttingDown) {
        if (isRequest) {
            sendError(extractId(msg), kErrInvalidRequest, "server is shutting down");
        }
        return true;
    }

    if (method == "initialize") {
        handleInitialize(st, msg);
        return true;
    }

    if (method == "initialized") {
        st.initialized = true;
        return true;
    }

    // 未初始化前，除 initialize/exit 外其它请求一律拒绝
    if (!st.initialized) {
        if (isRequest) {
            sendError(extractId(msg), kErrServerNotInitialized, "server not initialized");
        }
        return true;
    }

    if (method == "shutdown") {
        handleShutdown(st, msg);
        return true;
    }

    const json params = msg.value("params", json::object());

    if (method == "textDocument/didOpen") {
        handleDidOpen(st, params);
        return true;
    }
    if (method == "textDocument/didChange") {
        handleDidChange(st, params);
        return true;
    }
    if (method == "textDocument/didSave") {
        handleDidSave(st, params);
        return true;
    }
    if (method == "textDocument/didClose") {
        handleDidClose(st, params);
        return true;
    }

    if (method == "textDocument/documentSymbol") {
        handleDocumentSymbol(st, msg);
        return true;
    }
    if (method == "textDocument/formatting") {
        handleFormatting(st, msg);
        return true;
    }
    if (method == "textDocument/completion") {
        handleCompletion(st, msg);
        return true;
    }
    if (method == "textDocument/definition") {
        handleDefinition(st, msg);
        return true;
    }
    if (method == "textDocument/hover") {
        handleHover(st, msg);
        return true;
    }
    if (method == "textDocument/signatureHelp") {
        handleSignatureHelp(st, msg);
        return true;
    }
    if (method == "textDocument/semanticTokens/full") {
        handleSemanticTokensFull(st, msg);
        return true;
    }

    // P2+: 其它 textDocument/* 在此分发
    if (isRequest) {
        sendError(extractId(msg), kErrMethodNotFound, "method not found: " + method);
    }
    // 未知通知静默丢弃（LSP 规范允许）
    return true;
}

} // namespace

int runServer() {
    setStdioBinary();

    ServerState st;
    while (true) {
        auto raw = readMessage();
        if (!raw) {
            // EOF：客户端断开
            return 0;
        }
        if (raw->empty()) {
            // 协议错误（无 Content-Length）：跳过
            continue;
        }

        json msg;
        try {
            msg = json::parse(*raw);
        } catch (const json::parse_error& e) {
            sendError(nullptr, kErrParseError, std::string("parse error: ") + e.what());
            continue;
        }

        if (!msg.is_object()) {
            sendError(nullptr, kErrInvalidRequest, "message must be a JSON object");
            continue;
        }

        if (!dispatch(st, msg)) {
            // 收到 exit
            return st.shuttingDown ? 0 : 1;
        }
    }
}

} // namespace riu::lsp
