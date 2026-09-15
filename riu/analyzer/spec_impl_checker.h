// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// draft 显式实现校验器 (spec §12.2 / §12.3 / §12.5).
//
// 本文件提供 SpecImplChecker, 在 AST 全部解析完成后、codegen 之前
// 对所有 FileNode 上的 StructImplNode (含 specRefs 的 `Type : D1 + D2` 形态)
// 做语义检查:
//   - §12.2.2.1 穷尽性     E1101: 实现块缺 D 中的方法签名
//   - §12.2.2.1 不多余     E1102: 实现块出现非 D 签名集中的方法
//   - §12.2.2.2 重复       E1103: 同一 (typeQualified, specQualified+typeArgs) 多次出现
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

#ifndef RIU_LANG_DRAFT_IMPL_CHECKER_H
#define RIU_LANG_DRAFT_IMPL_CHECKER_H

#include "ast/node/file_node.h"
#include "ast/node/fn_node.h"
#include "ast/node/spec_node.h"
#include "ast/node/struct_node.h"
#include "types.h"

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

class Riu;

class SpecImplChecker {
public:
    explicit SpecImplChecker(Riu* riu);

    // 跑全套显式 draft 实现校验. 命中第一处错误即抛 RiuError.
    void validate();

    // §12.4 `#DraftLike` 结构化匹配 helper (Phase 3.2.d).
    // 给定类型裸名 + draft 声明 + draft 自身泛型实参, 判定该类型在
    // 当前 Riu 持有的所有 FileNode 中是否提供了与 draft 每个签名 §12.3.1
    // 等价的方法 (普通方法块与 draft 实现块的方法都计入). 仅返回 bool,
    // 不抛错 — 由 Phase 3.3 的边界匹配负责把 false 转成诊断.
    //
    // 注: 类型参数为 bare 名 (例如 "Counter"), 与 draft impl 解析使用的
    // 命名空间一致; 泛型类型实例化场景 (例如 `Counter<i32>`) 留给 3.3.
    bool typeSatisfiesSpec(const std::string& typeBareName, SpecDeclNode* draft,
                           const std::vector<TypeInfo>& specTypeArgs) const;

    // §6.4.4.4 / §12.4 泛型边界单态化校验 (Phase 3.3): 给定实参类型 + 一个
    // 已解析的 draft, 返回 typeArg 是否满足该 draft 边界. 命中条件:
    //   (a) 已存在显式实现 `Type : D<specTypeArgs> { ... }` (复用 _seen),
    //   (b) 或 D 标 `#DraftLike` 且 §12.3.1 结构匹配 (typeSatisfiesSpec).
    // 仅返回 bool, 由调用方负责把 false 翻成 E1106 诊断.
    //
    // 调用前需保证 validate() 已跑过 (Riu::specImplChecker() 自动触发).
    bool boundSatisfied(const TypeInfo& typeArg, SpecDeclNode* draft, const std::string& specQualified,
                        const std::vector<TypeInfo>& specTypeArgs) const;

    // §12.9 / DRAFT-dyn-draft §4 对象安全 (Phase 2a):
    // 给定 draft D, 判定其方法签名是否允许进入 Dyn<D> / Dyn<D&> 形态.
    // v1 第一轮规则: 任一 fnSig 的"非 receiver"参数类型 / 返回类型 (递归)
    // 出现 "Self" 字面量名, 或出现 draft 自身名, 均判 not object-safe.
    // 命中 false 由调用方翻成 E1134 诊断. 结果按 draft 节点指针 memoize.
    //
    // 注: riu 当前 fnSig 不显式承载 receiver (struct 上下文由 impl 提供),
    // 所以这里所有 params + retType 都视为"非 receiver"位.
    bool specIsObjectSafe(SpecDeclNode* draft) const;

    // §12.9 / DRAFT-dyn-draft 类型声明检查 (Phase 2c).
    // 遍历 SDK + 用户文件所有声明位的 TypeNode 子树:
    //   - fn 形参 / 返回类型 (free fn / impl 方法 / draft sig)
    //   - struct 字段类型
    //   - enum variant payload 类型
    //   - 顶层类型别名 target
    // 对每个 Dyn<X> 形态:
    //   E1131: X (剥外层 Ref 后) 必须是已知 draft 名
    //   E1132: X 不能再是 Dyn (Dyn<Dyn<...>>)
    //   E1132: 容器 Rc<Dyn<...>> / Weak<Dyn<...>> 禁
    //   E1135: Dyn<D>? (Nullable<Dyn<...>>) 不支持
    //   E1134: 内层 draft 必须对象安全
    // 命中第一处即抛 RiuError. 与构造表达式 E1131..E1134 的检查互补.
    void validateDynTypeReferences();

private:
    Riu* _riu;

    // 已登记的 (typeQualified, specQualifiedWithArgs) → impl, 用于 E1103.
    std::map<std::pair<std::string, std::string>, StructImplNode*> _seen;

    // §12.9 对象安全结果 memo: SpecDeclNode* → object-safe?  Phase 2a.
    mutable std::map<SpecDeclNode*, bool> _objectSafeCache;

    // structName → 所属模块名. 内置类型 (i32 / String / Rc ...) 归 "riu.core".
    std::map<std::string, std::string> _typeOwnerModule;

    // validateDynInTypeNode 跟随透明别名时的环检测（A = B, B = A）
    mutable std::set<std::string> _dynAliasVisited;

    void buildTypeOwnerMap();
    void validateImpl(FileNode* implFile, StructImplNode* impl);

    // §12.4.2.1 显隐冲突 E1105: 同 Type 普通方法块与 draft 实现块
    // 同名同签名共存. 在所有 impl 块依次校验完后调用, 遍历全局 impls
    // 配对比对; 命中即抛 RiuError.
    void checkExplicitImplicitConflict();

    // 按 §12.3.1 比较 impl 方法签名与 draft 签名. subst 为 draft 自身泛型
    // 形参 → impl 块给出的类型实参的替换表.
    bool sigEquivalent(FnHeaderNode* implMethod, FnHeaderNode* specSig,
                       const std::map<std::string, TypeInfo>& subst) const;

    // 拼 `<i32,String>` 形态尾缀, 让 `Counter : To<i32>` 与
    // `Counter : To<String>` 在 _seen 中视为不同 key (§12.3.2.3).
    static std::string specTypeArgsSuffix(const SpecRef& ref);

    // 类型 bare 名 → 所属模块. 未登记返回空串.
    std::string moduleOfType(const std::string& typeBareName) const;

    // Phase 2c helper: 递归校验一个 TypeNode 子树中的 Dyn 用法.
    // file 用于 draft 注册表按可见性解析裸名.
    // outerWrapper 标识当前 TypeNode 是否被某容器包裹 (空串表示根/允许容器):
    //   "Rc" / "Weak" / "Dyn" / "Nullable" → 命中 Dyn 时报相应错误码.
    //   "Array" / "Ref" / "" 等不触发包裹诊断 (Array<Dyn<D>> 合法; Dyn<D&> 内部 Ref 合法).
    void validateDynInTypeNode(TypeNode* tn, FileNode* file, const std::string& outerWrapper) const;
};

#endif // RIU_LANG_DRAFT_IMPL_CHECKER_H
