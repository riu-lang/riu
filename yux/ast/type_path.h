// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#ifndef YUX_LANG_TYPE_PATH_H
#define YUX_LANG_TYPE_PATH_H

#include "types.h"

class AliasDeclNode;
class EnumDeclNode;
class FileNode;
class StructDeclNode;
class Yux;

// 对已加载模块图解 TypePath（不含 arity / 方法重载）。不调用 loadModule：
// 只走已 use/load 的模块别名与 packageChild。裸名 L1 本文件 → L2 具名导入 →
// L3 通配（含默认 yux.core.*）。同层多候选（身份去重后）抛 E5015。
// 未解析时 type 仅末段短名、ownerModule 空（与 T2 前 getType 行为兼容）。
// frontend 的 NameResolver 包一层给 Sema；AST 节点直接调本函数。
struct TypePathResult {
    TypeInfo type;
    FileNode* owner = nullptr;
    StructDeclNode* structDecl = nullptr;
    EnumDeclNode* enumDecl = nullptr;
    AliasDeclNode* aliasDecl = nullptr;
    bool resolved = false;
};

TypePathResult resolveTypePath(FileNode* file, Yux* yux, const TypePath& path, int line = 0, int col = 0);

#endif // YUX_LANG_TYPE_PATH_H
