// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#include "symbol_lookup.h"

#include "workspace.h"
#include "../node/file_node.h"
#include "../node/fn_node.h"
#include "../node/struct_node.h"
#include "../node/global_const_node.h"
#include "../node/type_node.h"

#include "antlr4-runtime.h"
#include "yux/yuxLexer.h"
#include "utf8.h"

#include <algorithm>
#include <set>
#include <sstream>

namespace yux::lsp {

namespace {

// 把 (ANTLR 1-based 行 + 0-based 代码点列, token 文本) → LSP Range
// 行号对换：ANTLR 1-based → LSP 0-based
LspPosition toLspStart(const std::string& docText, size_t antlrLine, size_t antlrCol) {
    return antlrToLsp(docText, static_cast<int>(antlrLine), static_cast<int>(antlrCol));
}

LspPosition toLspEnd(const std::string& docText, size_t antlrLine, size_t antlrCol, const std::string& tokText) {
    // token 是单行 IDENT，结束列 = 起始列 + 代码点数
    int cps = 0;
    auto it = tokText.begin();
    auto end = tokText.end();
    try {
        while (it < end) {
            utf8::next(it, end);
            ++cps;
        }
    } catch (...) {
        cps = static_cast<int>(tokText.size());
    }
    return antlrToLsp(docText, static_cast<int>(antlrLine), static_cast<int>(antlrCol) + cps);
}

bool tokenContains(const LspPosition& s, const LspPosition& e, const LspPosition& p) {
    if (p.line < s.line || p.line > e.line) return false;
    if (p.line == s.line && p.character < s.character) return false;
    if (p.line == e.line && p.character > e.character) return false;
    return true;
}

bool readFileText(const std::string& absPath, std::string& out) {
    FILE* fp = std::fopen(absPath.c_str(), "rb");
    if (!fp) return false;
    std::fseek(fp, 0, SEEK_END);
    long sz = std::ftell(fp);
    std::fseek(fp, 0, SEEK_SET);
    out.resize(sz > 0 ? static_cast<size_t>(sz) : 0);
    if (sz > 0) std::fread(out.data(), 1, static_cast<size_t>(sz), fp);
    std::fclose(fp);
    return true;
}

std::string typeText(const p<TypeNode>& t) {
    if (!t) return "<?>";
    return typeInfoDisplay(t->getType());
}

} // namespace

std::string typeInfoDisplay(const TypeInfo& info) {
    if (info.empty()) return "<?>";
    if (info.kind == TypeKind::Generic && !info.genericArgs.empty()) {
        std::string s = info.name + "<";
        bool first = true;
        for (const auto& a : info.genericArgs) {
            if (!first) s += ", ";
            s += a ? typeInfoDisplay(*a) : "?";
            first = false;
        }
        s += ">";
        return s;
    }
    if (info.kind == TypeKind::Array && info.elementType) {
        return "[" + typeInfoDisplay(*info.elementType) + " * " +
               std::to_string(info.arraySize) + "]";
    }
    return info.name;
}

TokenHit identifierAt(const std::string& docText, LspPosition pos) {
    TokenHit hit;
    antlr4::ANTLRInputStream input(docText);
    ::yux::yuxLexer lexer(&input);
    lexer.removeErrorListeners();
    antlr4::CommonTokenStream tokens(&lexer);
    tokens.fill();
    for (auto* tok : tokens.getTokens()) {
        if (tok->getType() != ::yux::yuxLexer::ID) continue;
        if (tok->getChannel() != antlr4::Token::DEFAULT_CHANNEL) continue;
        size_t antlrLine = tok->getLine();
        size_t antlrCol = tok->getCharPositionInLine();
        std::string text = tok->getText();
        LspPosition s = toLspStart(docText, antlrLine, antlrCol);
        LspPosition e = toLspEnd(docText, antlrLine, antlrCol, text);
        if (tokenContains(s, e, pos)) {
            hit.text = std::move(text);
            hit.start = s;
            hit.end = e;
            hit.found = true;
            return hit;
        }
    }
    return hit;
}

LookupResult lookupName(Project& project, FileNode* fromFile, const std::string& name) {
    LookupResult r;
    if (!fromFile) return r;

    auto inspectFile = [&](FileNode* f) -> bool {
        if (!f) return false;
        // 优先：function 重载
        for (const auto& fn : f->getFunctions()) {
            if (fn->header()->name().getText() == name) {
                r.kind = LookupResult::Kind::Function;
                if (!r.fn) {
                    r.fn = fn;
                    r.ownerFile = f;
                }
                r.overloads.push_back(fn);
            }
        }
        if (r.kind == LookupResult::Kind::Function) return true;
        if (auto* sd = f->getStructDecl(name)) {
            r.kind = LookupResult::Kind::Struct;
            r.sd = sd;
            r.si = f->getStructImpl(name);
            r.ownerFile = f;
            return true;
        }
        for (const auto& gc : f->getGlobalConsts()) {
            if (gc->name().getText() == name) {
                r.kind = LookupResult::Kind::GlobalConst;
                r.gc = gc;
                r.ownerFile = f;
                return true;
            }
        }
        return false;
    };

    if (inspectFile(fromFile)) return r;

    // wildcard imports（含 yux.core 等隐式导入）
    for (auto* w : fromFile->wildcardImports()) {
        if (inspectFile(w)) return r;
    }
    // 同一项目下其它文件兜底（覆盖未显式导入的情况，给跳转一点容忍度）
    for (const auto& kv : project.allFiles()) {
        if (kv.second == fromFile) continue;
        if (inspectFile(kv.second)) return r;
    }
    return r;
}

LocatedRange locateNamedSymbol(FileNode* file, const std::string& name) {
    LocatedRange out;
    if (!file) return out;
    Token tok;
    bool gotToken = false;
    if (auto* fn = file->getFunction(name)) {
        tok = fn->header()->name();
        gotToken = true;
    } else if (auto* sd = file->getStructDecl(name)) {
        tok = sd->name();
        gotToken = true;
    } else {
        for (const auto& gc : file->getGlobalConsts()) {
            if (gc->name().getText() == name) {
                tok = gc->name();
                gotToken = true;
                break;
            }
        }
    }
    if (!gotToken) return out;
    // 文件路径 → uri
    // FileNode 不直接持文件路径；我们通过 Project::allFiles() 反查会更稳，但
    // 这里假定 caller 已知 file 的归一化路径并自己拼 URI。本函数只产 (line/col) 范围。
    // 行号 ANTLR 1-based → LSP 0-based；列 ANTLR 0-based 代码点 → 等价 0-based UTF-16
    //（标识符全 ASCII，不会有 UTF-16 surrogate）
    out.start.line = static_cast<int>(tok.getLine() > 0 ? tok.getLine() - 1 : 0);
    out.start.character = static_cast<int>(tok.getCharPositionInLine());
    out.end.line = out.start.line;
    out.end.character = out.start.character + static_cast<int>(tok.getText().size());
    out.ok = true;
    return out;
}

std::string renderFnHover(FnNode* fn) {
    if (!fn) return {};
    auto sig = renderSignature(fn);
    std::ostringstream ss;
    ss << "```yux\n" << sig.label << "\n```";
    return ss.str();
}

std::string renderStructHover(StructDeclNode* sd) {
    if (!sd) return {};
    std::ostringstream ss;
    ss << "```yux\nstruct " << sd->name().getText();
    if (sd->isGeneric()) {
        ss << "<";
        bool first = true;
        for (const auto& tp : sd->typeParams()) {
            if (!first) ss << ", ";
            ss << tp;
            first = false;
        }
        ss << ">";
    }
    ss << " {\n";
    for (const auto& f : sd->fields()) {
        ss << "    " << f->name().getText() << " " << typeText(f->type()) << "\n";
    }
    ss << "}\n```";
    return ss.str();
}

std::string renderConstHover(GlobalConstNode* gc) {
    if (!gc) return {};
    std::ostringstream ss;
    ss << "```yux\ncval " << gc->name().getText() << " " << typeInfoDisplay(gc->getType()) << "\n```";
    return ss.str();
}

SignatureLabels renderSignature(FnNode* fn) {
    SignatureLabels out;
    std::ostringstream ss;
    ss << "fn " << fn->header()->name().getText() << "(";
    int posBase = static_cast<int>(ss.tellp());
    bool first = true;
    for (const auto& p : fn->header()->params()) {
        if (!first) {
            ss << ", ";
            posBase = static_cast<int>(ss.tellp());
        }
        std::string piece = p->name().getText() + " " + typeText(p->type());
        int s = posBase;
        ss << piece;
        int e = static_cast<int>(ss.tellp());
        out.params.emplace_back(s, e);
        first = false;
    }
    ss << ")";
    auto retInfo = fn->header()->retType() ? fn->header()->retType()->getType() : TypeInfo();
    auto retStr = typeInfoDisplay(retInfo);
    if (!retStr.empty() && retStr != "void" && retStr != "<?>") ss << " " << retStr;
    out.label = ss.str();
    return out;
}

CompletionVisible collectVisible(Project& project, FileNode* fromFile) {
    CompletionVisible v;
    std::set<std::string> seenFn, seenStruct, seenConst;
    auto pushFile = [&](FileNode* f) {
        if (!f) return;
        for (const auto& fn : f->getFunctions()) {
            const auto& nm = fn->header()->name().getText();
            if (seenFn.insert(nm).second) v.functions.push_back(fn);
        }
        for (const auto& sd : f->getStructDecls()) {
            const auto& nm = sd->name().getText();
            if (seenStruct.insert(nm).second) v.structs.push_back(sd);
        }
        for (const auto& gc : f->getGlobalConsts()) {
            const auto& nm = gc->name().getText();
            if (seenConst.insert(nm).second) v.consts.push_back(gc);
        }
    };
    if (fromFile) {
        pushFile(fromFile);
        for (auto* w : fromFile->wildcardImports()) pushFile(w);
    }
    // 项目内其它文件作为补充（不严格按 use 可见性，宽松一些方便编辑）
    for (const auto& kv : project.allFiles()) {
        if (kv.second != fromFile) pushFile(kv.second);
    }
    return v;
}

CallContext findEnclosingCall(const std::string& docText, LspPosition pos) {
    CallContext ctx;
    // 把 LSP (line, utf16) 折回文本字节偏移
    size_t lineStart = 0;
    int curLine = 0;
    while (curLine < pos.line && lineStart < docText.size()) {
        if (docText[lineStart] == '\n') ++curLine;
        ++lineStart;
    }
    // 在该行内推进 pos.character 个 UTF-16 单元
    size_t byteOff = lineStart;
    int utf16 = 0;
    auto it = docText.begin() + static_cast<std::ptrdiff_t>(lineStart);
    auto end = docText.end();
    try {
        while (it < end && utf16 < pos.character) {
            char32_t cp = utf8::next(it, end);
            utf16 += (cp <= 0xFFFF ? 1 : 2);
        }
    } catch (...) {
        // 残缺 UTF-8 → 用当前 it 已走到的位置兜底
    }
    byteOff = static_cast<size_t>(it - docText.begin());

    // 从 byteOff 向前扫描，找未闭合的 '(' ；途中 ',' 计数；遇 ')' 嵌套加 1
    int depth = 0;
    int commas = 0;
    long i = static_cast<long>(byteOff) - 1;
    long openParen = -1;
    while (i >= 0) {
        char c = docText[static_cast<size_t>(i)];
        if (c == ')') {
            ++depth;
        } else if (c == '(') {
            if (depth == 0) {
                openParen = i;
                break;
            }
            --depth;
        } else if (c == ',' && depth == 0) {
            ++commas;
        }
        --i;
    }
    if (openParen < 0) return ctx;
    // 向前跳过空白，取标识符
    long j = openParen - 1;
    while (j >= 0) {
        char c = docText[static_cast<size_t>(j)];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') --j;
        else break;
    }
    long nameEnd = j + 1;
    while (j >= 0) {
        char c = docText[static_cast<size_t>(j)];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_') {
            --j;
        } else break;
    }
    long nameStart = j + 1;
    if (nameEnd <= nameStart) return ctx;
    ctx.callee = docText.substr(static_cast<size_t>(nameStart),
                                 static_cast<size_t>(nameEnd - nameStart));
    if (ctx.callee.empty() || (ctx.callee[0] >= '0' && ctx.callee[0] <= '9')) {
        ctx.callee.clear();
        return ctx;
    }
    ctx.activeParam = commas;
    ctx.found = true;
    return ctx;
}

} // namespace yux::lsp
