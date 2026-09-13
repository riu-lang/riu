// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#ifndef YUX_LANG_SEMA_NAME_RESOLVER_H
#define YUX_LANG_SEMA_NAME_RESOLVER_H

#include "ast/node/file_node.h"
#include "ast/type_path.h"

class AliasDeclNode;
class EnumDeclNode;
class StructDeclNode;
class Yux;
struct FnSymbolInfo;
struct TypeInfo;
struct TypePath;

// 跨文件名字查找（0 LLVM）：L1 本文件本地 → L2 具名导入 → SDK / L3 wildcard。
// FileNode::get* 已含本文件 wildcard；此处再补 SDK，并保留第三段
// `imp->get*`（导入文件自己的 wildcard，与历史 lookupEnumDecl 一致）。
namespace sema {

struct NameResolver {
    FileNode* file = nullptr;
    FileNode* sdkFile = nullptr;

    NameResolver() = default;
    NameResolver(FileNode* f, FileNode* sdk) : file(f), sdkFile(sdk) {}

    // includeBuiltin：是否命中 `#Builtin` 占位 struct（Rc/Array 等）。Sema arity 校验传 true。
    [[nodiscard]] StructDeclNode* lookupStruct(const string& name, bool includeBuiltin = false,
                                               FileNode** outOwner = nullptr) const;
    // ownerModule 非空时只在该模块的本地声明里找（身份消歧）
    [[nodiscard]] StructDeclNode* lookupStruct(const TypeInfo& t, bool includeBuiltin = false,
                                               FileNode** outOwner = nullptr) const;
    [[nodiscard]] EnumDeclNode* lookupEnum(const string& name, FileNode** outOwner = nullptr) const;
    [[nodiscard]] EnumDeclNode* lookupEnum(const TypeInfo& t, FileNode** outOwner = nullptr) const;
    [[nodiscard]] AliasDeclNode* lookupAlias(const string& name, FileNode** outOwner = nullptr) const;
    [[nodiscard]] FnSymbolInfo* lookupFn(const string& name, FileNode** outOwner = nullptr) const;
    [[nodiscard]] FnSymbolInfo* lookupFnWithParams(const string& name, const vector<TypeInfo>& paramTypes,
                                                   FileNode** outOwner = nullptr) const;
    // 已加载模块中按 moduleName 找 FileNode；不 loadModule
    [[nodiscard]] FileNode* fileForOwner(const string& ownerModule) const;
    [[nodiscard]] StructImplNode* lookupStructImpl(const string& name, FileNode** outOwner = nullptr) const;
    [[nodiscard]] StructImplNode* lookupStructImpl(const TypeInfo& t, FileNode** outOwner = nullptr) const;
    // recv.ownerModule 非空时只在该模块查 `Name.method`
    [[nodiscard]] FnSymbolInfo* lookupMethod(const TypeInfo& recv, const string& methodName,
                                             FileNode** outOwner = nullptr) const;
    [[nodiscard]] FnSymbolInfo* lookupMethodWithParams(const TypeInfo& recv, const string& methodName,
                                                       const vector<TypeInfo>& paramTypes,
                                                       FileNode** outOwner = nullptr) const;
};

// 透明别名解析（完整版：递归 generic / array / tuple / fn）。遇环抛 E2016。
[[nodiscard]] TypeInfo resolveAlias(const TypeInfo& t, FileNode* file, FileNode* sdkFile = nullptr);

// 顶层别名一次性校验：E2017 名字冲突 + E2016 环 + fn 符号表归一化。
// SemaPass::run 起始处调一次；Compiler 不再双跑。
void validateAliases(FileNode* file, FileNode* sdkFile = nullptr);

// 实现在 ast/type_path.cpp（已加载模块图，不含 arity / 重载）。此处转发给 Sema。
using ::resolveTypePath;
using ::TypePathResult;

// 表达式 LHS：resolveTypePath 后再展开别名，填 structDecl / enumDecl。
// Self 由调用方处理，不要把 "Self" 丢进来当路径。
TypePathResult resolveExprTypeLhs(FileNode* file, Yux* yux, const TypePath& path, int line = 0, int col = 0);

} // namespace sema

#endif // YUX_LANG_SEMA_NAME_RESOLVER_H
