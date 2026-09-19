// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#ifndef RIU_LANG_NAME_LOOKUP_H
#define RIU_LANG_NAME_LOOKUP_H

#include "ast/node/file_node.h"
#include "ast/type_path.h"

#include <map>

class AliasDeclNode;
class EnumDeclNode;
class StructDeclNode;
class StructImplNode;
class Node;
class Riu;
struct FnSymbolInfo;
struct TypeInfo;
struct TypePath;

// 已加载模块图上的跨文件名字查找（0 LLVM）：L1 本文件本地 → L2 具名导入 → SDK / L3
// wildcard。不含 arity / 方法重载消歧。放 ast：structuralType 无槽回退与 Sema 共用，
// 避免 riu_ast include frontend。仍用 namespace sema，调用点不改名。
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
// cycleFrom 非空时先插入（validateAliases：从别名自身起算环，报错行落在该声明）。
[[nodiscard]] TypeInfo resolveAlias(const TypeInfo& t, FileNode* file, FileNode* sdkFile = nullptr,
                                    const string& cycleFrom = {});

// 实现在 ast/type_path.cpp（已加载模块图，不含 arity / 重载）。
using ::resolveTypePath;
using ::TypePathResult;

// 表达式 LHS：resolveTypePath 后再展开别名，填 structDecl / enumDecl。
// Self 由调用方处理，不要把 "Self" 丢进来当路径。
TypePathResult resolveExprTypeLhs(FileNode* file, Riu* riu, const TypePath& path, int line = 0, int col = 0);
// from 非空时，裸名先查块 / struct 内 `type`（可遮蔽文件顶层）。
TypePathResult resolveExprTypeLhs(const Node* from, FileNode* file, Riu* riu, const TypePath& path, int line = 0,
                                  int col = 0);

// 沿 parent 链查块 / struct 局部别名；跳过 FileNode（顶层走 _aliasMap / resolveAlias）。
[[nodiscard]] AliasDeclNode* lookupScopedAlias(const Node* from, const string& name);
// 展开局部别名目标；成环抛 E2016。
[[nodiscard]] TypeInfo expandScopedAlias(const AliasDeclNode* alias);

// 泛型 enum 单态：形参 → enumType.genericArgs。非泛型或实参个数不对返回空 map。
// match 绑定 / 穷尽诊断用带实参的 enumType，不能只拿声明 payload 原文。
std::map<std::string, TypeInfo> enumInstSubst(EnumDeclNode* enumDecl, const TypeInfo& enumType);

} // namespace sema

#endif // RIU_LANG_NAME_LOOKUP_H
