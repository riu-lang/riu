// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// 定位/查找辅助：
// - 在文档文本里找指定 LSP Position 处的标识符 token
// - 在 Project FileNode 里按名字查 fn / struct / letGlobal → 定位 + 渲染

#pragma once

#include <string>
#include <vector>

#include "position.h"
#include "types.h"

class FileNode;
class FnNode;
class StructDeclNode;
class StructFieldNode;
class GlobalConstNode;
class StructImplNode;
struct TypeInfo;

namespace yux::lsp {

class Project;

// 在 docText 中找位于 pos 的 IDENT token；找不到 token.empty()
struct TokenHit {
    std::string text;
    LspPosition start;
    LspPosition end;
    bool found = false;
};
TokenHit identifierAt(const std::string& docText, LspPosition pos);

// 名字查找：先 file 本地，再依次走 wildcardImports / moduleAliases / packageAliases
struct LookupResult {
    enum class Kind : u8 { None, Function, Struct, GlobalConst };
    Kind kind = Kind::None;
    FileNode* ownerFile = nullptr;
    FnNode* fn = nullptr;
    StructDeclNode* sd = nullptr;
    StructImplNode* si = nullptr; // 仅 Struct 时附带（可能为 nullptr）
    GlobalConstNode* gc = nullptr;
    std::vector<FnNode*> overloads; // Function 时收集所有同名 fn（含跨文件）
};
LookupResult lookupName(Project& project, FileNode* fromFile, const std::string& name);

// 计算名字 token 在某个源文件里的 LSP Range（基于 Token 的 line/col + UTF-16 列换算）
// 文件文本通过 readSourceText 现读现解；找不到文件返回 {0,0}-{0,0}
struct LocatedRange {
    std::string fileUri;
    LspPosition start;
    LspPosition end;
    bool ok = false;
};
LocatedRange locateNamedSymbol(FileNode* file, const std::string& name);

// 渲染 hover markdown
std::string renderFnHover(FnNode* fn);
std::string renderStructHover(StructDeclNode* sd);
std::string renderConstHover(GlobalConstNode* gc);

// 把 TypeInfo 渲染成 yux 源码风格（如 Foo<i32> / [i32 * 4]）
std::string typeInfoDisplay(const TypeInfo& info);

// 用于 signatureHelp：把 fn 渲染成签名字符串与参数 label 范围列表
struct SignatureLabels {
    std::string label;                       // "fn name(p1 T1, p2 T2) Ret"
    std::vector<std::pair<int, int>> params; // 每个参数在 label 中 [start, end) 字符偏移
};
SignatureLabels renderSignature(FnNode* fn);

// 当前文件中可见的所有名字（用于动态补全）
struct CompletionVisible {
    std::vector<FnNode*> functions;
    std::vector<StructDeclNode*> structs;
    std::vector<GlobalConstNode*> consts;
};
CompletionVisible collectVisible(Project& project, FileNode* fromFile);

// 受 cursor 控制的 `receiver.member` 上下文（仅识别 receiver 是单 IDENT 的情形）。
// 用法：definition / completion 命中 IDENT 时先查这个；不命中再走 lookupName。
//   - definition：cursor 落在 member 上 → member 范围有效
//   - completion：cursor 在 `.` 之后（member 可空）→ memberStart/End 为 cursor 自身
struct ReceiverCtx {
    bool found = false;
    std::string receiver;    // dot 左侧的 IDENT
    std::string member;      // dot 右侧的 IDENT；completion 后缀为空时为 ""
    LspPosition memberStart; // 仅 member 非空时有意义
    LspPosition memberEnd;
};
// member 模式：cursor 在 IDENT 内/末，IDENT 前为 `\s*\.\s*IDENT`
ReceiverCtx receiverContextAt(const std::string& docText, LspPosition pos);

// 在 fromFile（含其 wildcardImports）找名为 structName 的 struct，返回成员定位
struct MemberHit {
    enum class Kind : u8 { None, Field, Method } kind = Kind::None;
    FileNode* ownerFile = nullptr;
    StructFieldNode* field = nullptr;     // Field 时有效
    FnNode* method = nullptr;             // Method 时取首个重载
    std::vector<FnNode*> methodOverloads; // Method 时全部同名重载
};
MemberHit lookupStructMember(Project& project, FileNode* fromFile, const std::string& structName,
                             const std::string& memberName);

struct StructMembers {
    std::vector<StructDeclNode*> structs; // 命中的 struct 声明（用于字段渲染）
    std::vector<FnNode*> methods;         // 该 struct 的所有方法（含重载）
};
StructMembers collectStructMembers(Project& project, FileNode* fromFile, const std::string& structName);

// 在文档文本中向左扫描定位"当前正在调用的函数名"（用于 signatureHelp）
// 返回 (callee, activeParameter)；找不到则 found=false
struct CallContext {
    std::string callee;
    int activeParam = 0;
    bool found = false;
};
CallContext findEnclosingCall(const std::string& docText, LspPosition pos);

} // namespace yux::lsp
