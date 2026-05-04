// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// draft 显式实现校验器 (spec §12.2 / §12.3 / §12.5).
//
// 本文件提供 DraftImplChecker, 在 AST 全部解析完成后、codegen 之前
// 对所有 FileNode 上的 StructImplNode (含 draftRefs 的 `Type : D1 + D2` 形态)
// 做语义检查:
//   - §12.2.2.1 穷尽性     E1101: 实现块缺 D 中的方法签名
//   - §12.2.2.1 不多余     E1102: 实现块出现非 D 签名集中的方法
//   - §12.2.2.2 重复       E1103: 同一 (typeQualified, draftQualified+typeArgs) 多次出现
//   - §12.5    orphan      E1120: Type / D 都不在 impl 所在包
//   - §12.3.1  签名等价: 方法名 / 形参类型逐位 / 返回类型 (receiver 不参与,
//               参数名不参与); 应用 draft 自身泛型形参替换.
//
// 显隐冲突 (E1105, Phase 3.2.c): 同一 Type 同时被普通方法块 `m()` 与
// `Type:D{m()}` 实现, 且两侧 §12.3.1 签名等价 → 调用 obj.m() 歧义,
// 由本 checker 统一报告. 本 checker 不负责 `#DraftLike` 结构化匹配
// (Phase 3.2.d / 3.3).
//
// 入口 (Phase 3.2.e): Compiler::compile() 起始处构造一次, 调 validate().

#ifndef YUX_LANG_DRAFT_IMPL_CHECKER_H
#define YUX_LANG_DRAFT_IMPL_CHECKER_H

#include "node/draft_node.h"
#include "node/file_node.h"
#include "node/fn_node.h"
#include "node/struct_node.h"
#include "types.h"

#include <map>
#include <string>
#include <utility>
#include <vector>

class Yux;

class DraftImplChecker {
public:
    explicit DraftImplChecker(Yux* yux);

    // 跑全套显式 draft 实现校验. 命中第一处错误即抛 YuxError.
    void validate();

    // §12.4 `#DraftLike` 结构化匹配 helper (Phase 3.2.d).
    // 给定类型裸名 + draft 声明 + draft 自身泛型实参, 判定该类型在
    // 当前 Yux 持有的所有 FileNode 中是否提供了与 draft 每个签名 §12.3.1
    // 等价的方法 (普通方法块与 draft 实现块的方法都计入). 仅返回 bool,
    // 不抛错 — 由 Phase 3.3 的边界匹配负责把 false 转成诊断.
    //
    // 注: 类型参数为 bare 名 (例如 "Counter"), 与 draft impl 解析使用的
    // 命名空间一致; 泛型类型实例化场景 (例如 `Counter<i32>`) 留给 3.3.
    bool typeSatisfiesDraft(const std::string& typeBareName,
                            DraftDeclNode* draft,
                            const std::vector<TypeInfo>& draftTypeArgs) const;

private:
    Yux* _yux;

    // 已登记的 (typeQualified, draftQualifiedWithArgs) → impl, 用于 E1103.
    std::map<std::pair<std::string, std::string>, StructImplNode*> _seen;

    // structName → 所属模块名. 内置类型 (i32 / String / Box ...) 归 "yux.core".
    std::map<std::string, std::string> _typeOwnerModule;

    void buildTypeOwnerMap();
    void validateImpl(FileNode* implFile, StructImplNode* impl);

    // §12.4.2.1 显隐冲突 E1105: 同 Type 普通方法块与 draft 实现块
    // 同名同签名共存. 在所有 impl 块依次校验完后调用, 遍历全局 impls
    // 配对比对; 命中即抛 YuxError.
    void checkExplicitImplicitConflict();

    // 按 §12.3.1 比较 impl 方法签名与 draft 签名. subst 为 draft 自身泛型
    // 形参 → impl 块给出的类型实参的替换表.
    bool sigEquivalent(FnHeaderNode* implMethod,
                       FnHeaderNode* draftSig,
                       const std::map<std::string, TypeInfo>& subst) const;

    // 拼 `<i32,String>` 形态尾缀, 让 `Counter : To<i32>` 与
    // `Counter : To<String>` 在 _seen 中视为不同 key (§12.3.2.3).
    static std::string draftTypeArgsSuffix(const DraftRef& ref);

    // 类型 bare 名 → 所属模块. 未登记返回空串.
    std::string moduleOfType(const std::string& typeBareName) const;
};

#endif //YUX_LANG_DRAFT_IMPL_CHECKER_H
