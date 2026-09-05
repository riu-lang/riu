// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// draft 显式实现校验器 — 实现 (spec §12).
//
// 本文件实现:
//   - SpecImplChecker::validate(): 遍历 SDK + 用户文件中所有
//     带 specRefs 的 StructImplNode, 逐一调 validateImpl.
//   - validateImpl: 解析每个 D 的 qualifiedName, 顺序做 orphan / 重复 /
//     穷尽匹配 / 不多余检查; 对多 D 实现块, 不多余检查在所有 D 都比对
//     完后再统一判定 (任一 D 命中即视为合法).
//   - sigEquivalent: §12.3.1 等价判定 — 名 / 形参类型 / 返回类型;
//     先对 draft 侧应用 typeParam → impl 块给出的类型实参替换.

#include "spec_impl_checker.h"

#include "ast/node/alias_node.h"
#include "ast/node/enum_node.h"
#include "ast/node/type_node.h"
#include "ast/yux.h"
#include "error_code.h"
#include "spec_registry.h"

#include <functional>
#include <set>
#include <tuple>
#include <vector>

namespace {

// 内置类型按 yux.core 归属 (§12.5 orphan 用; SDK base.yux 内为这些类型
// 实现 ToString 等内置 draft 视为合法).
const std::set<std::string>& builtinTypeNames() {
    static const std::set<std::string> s = {"i8",      "u8",   "i16", "u16",      "i32",    "u32",           "i64",
                                            "u64",     "f32",  "f64", "bool",     "String", "StringBuilder", "Rc",
                                            "Array",   "Weak", "Ptr", "Nullable", "Ref",    "isize",         "usize",
                                            "Function"};
    return s;
}

} // namespace

SpecImplChecker::SpecImplChecker(Yux* yux) : _yux(yux) {}

void SpecImplChecker::buildTypeOwnerMap() {
    _typeOwnerModule.clear();
    auto index = [&](FileNode* file) {
        if (!file) return;
        for (auto& d : file->getStructDecls()) {
            // name() 按值返回临时 Token, 不能 bind 引用 (悬挂).
            std::string name = d->name().getText();
            // 同名跨文件冲突由其它阶段诊断, 这里取首次登记的 owner.
            _typeOwnerModule.emplace(std::move(name), file->moduleName());
        }
    };
    if (_yux) {
        if (auto sdk = _yux->sdkFile()) index(sdk);
        for (auto& f : _yux->files())
            index(f);
    }
}

std::string SpecImplChecker::moduleOfType(const std::string& bareName) const {
    auto it = _typeOwnerModule.find(bareName);
    if (it != _typeOwnerModule.end()) return it->second;
    if (builtinTypeNames().count(bareName)) return "yux.core";
    return {};
}

void SpecImplChecker::validate() {
    if (!_yux) return;
    // 每次 validate 都重新构建状态：上次跑过后若有新文件加入 (典型场景:
    // `yux test` 冷启动 — compileSdkDir 触发第一次 validate, 此时 test 文件还未
    // loadMainFile; 之后 test 文件加入后 boundSatisfied 需要看到 Tag:ToString
    // 这类 user-type impl). 不清会让 _seen 把上轮已登记的 (Type, Draft) 误判
    // 为 E1103 重复.
    _seen.clear();
    _objectSafeCache.clear();
    buildTypeOwnerMap();
    // 触发一次 registry 构建 (若未构建)
    (void)_yux->specRegistry();

    auto run = [&](FileNode* file) {
        if (!file) return;
        for (auto& impl : file->getStructImpls()) {
            // 普通方法块 (不带 `: D`) 不在本 checker 范围.
            if (impl->specRefs().empty()) continue;
            validateImpl(file, impl);
        }
    };
    // SDK 平铺文件拆分后，_sdkFile 为薄层空壳，struct impl 实际在各子文件
    // （base.yux / assert.yux 等）中。收集到 processedSet 去重，避免 SDK 编译
    // 时 _yux->files() 与 sdk->wildcardImports() 重叠导致 E1103 误报。
    set<FileNode*> processedSet;
    auto runOnce = [&](FileNode* file) {
        if (!file || !processedSet.insert(file).second) return;
        run(file);
    };
    if (auto sdk = _yux->sdkFile()) {
        runOnce(sdk);
        for (auto* imp : sdk->wildcardImports()) {
            runOnce(imp);
        }
    }
    for (auto& f : _yux->files())
        runOnce(f);

    // §12.4.2.1 E1105 显隐冲突: 必须等所有显式 impl 全部 §12.2 校验通过
    // 后再做, 避免"穷尽性 / 不多余" 与 显隐冲突 互相覆盖错误位置.
    checkExplicitImplicitConflict();

    // §12.9 Phase 2c: 类型声明位 Dyn 用法静态检查
    validateDynTypeReferences();
}

void SpecImplChecker::checkExplicitImplicitConflict() {
    // 每个 Type 收集普通方法块与 draft 实现块的方法; 配对比对签名等价.
    // 跨包条款 (§12.4.2.2 / §12.4.2.3) 已由 #DraftLike 路径与 §12.5
    // orphan 处理: 这里所见的"普通方法块 + draft 实现块共存"场景, 都是
    // §12.4.2.1 情形 A, 直接报 E1105.
    struct PerType {
        // (impl 块所在文件, 方法 header)
        std::vector<std::pair<FileNode*, FnHeaderNode*>> plain;
        // (impl 块所在文件, 方法 header, 该 impl 块首个 specRef 的限定名 — 仅用于错误信息)
        std::vector<std::tuple<FileNode*, FnHeaderNode*, std::string>> draft;
    };
    std::map<std::string, PerType> table;

    auto specQualifiedFor = [&](FileNode* file, StructImplNode* impl) -> std::string {
        if (impl->specRefs().empty()) return {};
        auto& reg = _yux->specRegistry();
        const auto& dref = impl->specRefs().front();
        if (auto r = reg.resolve(dref.name, file)) return r->qualifiedName;
        return dref.name;
    };

    auto collect = [&](FileNode* file) {
        if (!file) return;
        for (auto& impl : file->getStructImpls()) {
            const std::string& typeBare = impl->structName();
            const std::string ownerMod = moduleOfType(typeBare);
            std::string typeQualified;
            if (ownerMod.empty()) {
                typeQualified = typeBare;
            } else {
                typeQualified = ownerMod;
                typeQualified += '.';
                typeQualified += typeBare;
            }
            auto& slot = table[typeQualified];
            if (impl->specRefs().empty()) {
                for (auto& m : impl->methods()) {
                    slot.plain.emplace_back(file, m->header());
                }
            } else {
                std::string dq = specQualifiedFor(file, impl);
                for (auto& m : impl->methods()) {
                    slot.draft.emplace_back(file, m->header(), dq);
                }
            }
        }
    };
    if (auto sdk = _yux->sdkFile()) collect(sdk);
    for (auto& f : _yux->files())
        collect(f);

    // 对每个 Type, 若任一 (plain 方法, draft 方法) 同名 + §12.3.1 签名等价
    // 则报 E1105. 错误位点取 plain 一侧 — 通常这是用户已有的"普通定义",
    // 后写的 draft 实现更可能是新添加并触发冲突的一方; 以 plain 行号让
    // 错误信息聚焦在"已存在的成员".
    for (auto& [typeQualified, slot] : table) {
        for (auto& [pf, pm] : slot.plain) {
            for (auto& [df, dm, dq] : slot.draft) {
                if (pm->name().getText() != dm->name().getText()) continue;
                // 两侧均为已写出的具体方法, draft 自身泛型替换不需要
                // (draft 方法已按 §12.2.2 校验为 D 签名集的实现).
                if (sigEquivalent(pm, dm, {})) {
                    throw YuxError(pm->getLineNumber(), pm->getColumn(), ErrorCode::E1105, pm->name().getText(),
                                   typeQualified, dq);
                }
            }
        }
    }
}

void SpecImplChecker::validateImpl(FileNode* implFile, StructImplNode* impl) {
    auto& reg = _yux->specRegistry();
    const std::string typeBare = impl->structName();
    const std::string typeOwnerMod = moduleOfType(typeBare);
    const std::string typeQualified = typeOwnerMod.empty() ? typeBare : (typeOwnerMod + "." + typeBare);

    const auto& implMethods = impl->methods();

    // DRAFT-spec-default-body Phase 3: 每次 validateImpl 都清一遍, 否则
    // 多次 validate (例: 冷启动 SDK 跑一次 → loadMainFile 后再跑一次) 会累积重复.
    impl->clearInheritedDefaults();

    // DRAFT-spec-default-body Phase 4: 跨 spec 聚合每个 (name, arity) 组的
    // 所有签名条目, 二轮处理: 实现命中→OK; 全无默认体→E1101;
    // 多个默认体→E3132; 单一默认体→fall-through.
    // NOLINTNEXTLINE(bugprone-exception-escape) — std::map member may throw on copy; intentional
    struct SigEntry {
        SpecDeclNode* spec;
        size_t sigIdx;
        std::map<std::string, TypeInfo> subst;
        std::string specQualified;
    };
    std::map<std::pair<std::string, size_t>, std::vector<SigEntry>> sigGroups;

    for (auto& dref : impl->specRefs()) {
        auto resolved = reg.resolve(dref.name, implFile);
        if (!resolved) {
            // §10 名字解析失败. 复用 E3032 (未来可专门给一个 E11xx 码).
            throw YuxError(dref.line, dref.col, ErrorCode::E3030, dref.name);
        }
        SpecDeclNode* draft = resolved->decl;
        const std::string& specQualified = resolved->qualifiedName;
        const std::string specKey = specQualified + specTypeArgsSuffix(dref);

        // §12.2.2.2 重复
        auto key = std::make_pair(typeQualified, specKey);
        auto seenIt = _seen.find(key);
        if (seenIt != _seen.end()) {
            throw YuxError(impl->getLineNumber(), impl->getColumn(), ErrorCode::E1103, typeQualified, specKey);
        }
        _seen.emplace(key, impl);

        // §12.5 orphan: implMod ∈ {typeOwnerMod, specOwnerMod} 之一即合法.
        // spec-unify v1: #Impl(D) struct X 解出的 StructImplNode 必与 struct X
        // 同 FileNode, 故只需检查 implFile 本地是否声明同名 struct, 即可绕过
        // _typeOwnerModule 全局 bareName 折叠 (跨 test 文件同名 struct 时
        // 首登记 wins → 错认 owner module, 详 BUGS.md#4).
        bool typeLocalToImplFile = false;
        for (auto& d : implFile->getStructDecls()) {
            if (d->name().getText() == typeBare) {
                typeLocalToImplFile = true;
                break;
            }
        }
        const std::string specOwnerMod = resolved->ownerFile ? resolved->ownerFile->moduleName() : std::string();
        const std::string& implMod = implFile->moduleName();
        // implMod 为空 (单文件主入口未登记模块名) 时, 跳过 orphan 检查避免误报.
        if (!implMod.empty() && !typeLocalToImplFile && implMod != typeOwnerMod && implMod != specOwnerMod) {
            throw YuxError(impl->getLineNumber(), impl->getColumn(), ErrorCode::E1120, specQualified, typeQualified);
        }

        // 构造 draft 自身泛型形参 → 实参替换表.
        // 形参 / 实参 arity 不一致暂不在此处报 (留给 turbofish 解析层),
        // 此时 subst 只覆盖能配上的前缀, 后续 sig 比对自然失败.
        std::map<std::string, TypeInfo> subst;
        const auto& dParams = draft->typeParams();
        size_t n = std::min(dParams.size(), dref.typeArgs.size());
        for (size_t i = 0; i < n; ++i) {
            subst[dParams[i]] = dref.typeArgs[i];
        }
        // spec 体内 `Self` 占位符号 (TypeSelfNode 在 spec scope 内 structName 为空,
        // getType 返回空 TypeInfo, 名字为 "Self" — 用此映射让 sigEquivalent 把
        // spec `Self&` 与 impl `Type&` 视为同型). Phase 3 Spec 默认体 fall-through
        // 也共用同一签名等价规则.
        subst["Self"] = TypeInfo(typeBare);

        // Phase 4: 收集本 spec 的所有签名条目到 sigGroups; 实际处理放二轮.
        const auto& dsigs = draft->signatures();
        for (size_t sigIdx = 0; sigIdx < dsigs.size(); ++sigIdx) {
            auto& dsig = dsigs[sigIdx];
            // 注意: FnHeaderNode::name() 按值返回 Token, getText() 是其成员引用;
            // 不能写成 `const std::string& dname = dsig->name().getText();` ——
            // 临时 Token 在 full-expression 后销毁, dname 立即悬挂.
            const std::string dname = dsig->name().getText();
            size_t arity = dsig->params().size();
            sigGroups[{dname, arity}].push_back(
                {.spec = draft, .sigIdx = sigIdx, .subst = subst, .specQualified = specQualified});
        }
    }

    // Phase 4 二轮: 对每个 (name, arity) 组做命中 / fall-through / 冲突判定.
    // spec-unify v1: 同 struct body 内未命中任一 spec sig 的方法视为普通方法
    // 放行 (E1102 已废, 严格隔离待 extension blocks 草案).
    for (auto& [groupKey, entries] : sigGroups) {
        const std::string& name = groupKey.first;

        // 实现命中: 任一 impl 方法与该组任一 entry §12.3.1 等价 → 视为已实现.
        bool implemented = false;
        for (size_t i = 0; i < implMethods.size() && !implemented; ++i) {
            auto& m = implMethods[i]->header();
            if (m->name().getText() != name) continue;
            for (auto& e : entries) {
                auto& sig = e.spec->signatures()[e.sigIdx];
                if (sigEquivalent(m, sig, e.subst)) {
                    implemented = true;
                    break;
                }
            }
        }
        if (implemented) continue;

        // 未实现: 看默认体情况.
        std::vector<SigEntry*> withDefault;
        for (auto& e : entries) {
            if (e.spec->hasDefaultBody(e.sigIdx)) withDefault.push_back(&e);
        }

        if (withDefault.empty()) {
            // E1101: 取首条 entry 做诊断 (合并掉草案中的 E1136).
            auto& e = entries.front();
            throw YuxError(impl->getLineNumber(), impl->getColumn(), ErrorCode::E1101, typeQualified, e.specQualified,
                           name);
        }

        if (withDefault.size() >= 2) {
            // E3132: 组合冲突, 实现者必须显式覆盖. 一种默认体 + 一种纯抽象
            // 的情形被上面 withDefault.size() == 1 分支放行 (默认体顶上).
            std::string specList;
            for (size_t i = 0; i < withDefault.size(); ++i) {
                if (i) specList += ", ";
                specList += '`';
                specList += withDefault[i]->specQualified;
                specList += '`';
            }
            throw YuxError(impl->getLineNumber(), impl->getColumn(), ErrorCode::E3132, typeQualified, name, specList);
        }

        // 恰好一条默认体: 注册 fall-through (覆盖本组所有 spec).
        // 同名但与 spec 签名不等价的实现方法当作"另一个方法"看待, 仍可触发 fall-through.
        SigEntry* picked = withDefault[0];
        auto& dsig = picked->spec->signatures()[picked->sigIdx];
        impl->addInheritedDefault({.spec = picked->spec, .sigIdx = picked->sigIdx, .subst = picked->subst});

        // 同步把 fall-through 方法注册到 impl 所在 file 的 fnSymbol 表,
        // 让 ExprCallNode::getType 能解析 `obj.lt(...)`.
        std::string fullName = typeBare;
        fullName += '.';
        fullName += name;
        std::vector<TypeInfo> paramTypes;
        paramTypes.emplace_back(typeBare);
        for (auto& sp : dsig->params()) {
            TypeInfo pt = sp->type() ? sp->type()->getType() : TypeInfo();
            paramTypes.push_back(pt.substitute(picked->subst));
        }
        TypeInfo retType = dsig->retType() ? dsig->retType()->getType() : TypeInfo();
        retType = retType.substitute(picked->subst);
        SymbolInfo methodSym(SymbolKind::Function, name, retType);
        methodSym.moduleName = implFile->moduleName();
        implFile->registerSymbol(fullName, methodSym);
        FnSymbolInfo methodFnSym{fullName, implFile->moduleName(), paramTypes, retType};
        methodFnSym.isNoReturn = dsig->hasAnno("NoReturn");
        methodFnSym.isConst = dsig->hasAnno("Const");
        methodFnSym.fallibleErrType = dsig->resolvedFallibleErr();
        implFile->registerFnSymbol(fullName, methodFnSym);
    }

    // DRAFT-spec-disambig-at: 为每个 (spec, 默认体 method) 预登记 @-tagged 发射点.
    // 即便 impl 覆盖了同名方法 (escape hatch), `$.m@SpecA()` 也直接命中此符号 -> spec 默认体.
    // emitMethodName = origName + "@" + dref.name (单名, 与 g4 `@ID` 单名约束对齐).
    impl->clearSpecDisambigEmits();
    for (auto& dref : impl->specRefs()) {
        auto resolved = reg.resolve(dref.name, implFile);
        if (!resolved) continue;
        SpecDeclNode* spec = resolved->decl;

        std::map<std::string, TypeInfo> subst;
        const auto& dParams = spec->typeParams();
        size_t pn = std::min(dParams.size(), dref.typeArgs.size());
        for (size_t i = 0; i < pn; ++i) {
            subst[dParams[i]] = dref.typeArgs[i];
        }
        subst["Self"] = TypeInfo(typeBare);

        const auto& dsigs = spec->signatures();
        for (size_t sigIdx = 0; sigIdx < dsigs.size(); ++sigIdx) {
            if (!spec->hasDefaultBody(sigIdx)) continue;
            auto& dsig = dsigs[sigIdx];
            std::string origName = dsig->name().getText();
            origName += "@";
            origName += dref.name;
            std::string emitName = origName;
            std::string fullName2 = typeBare;
            fullName2 += ".";
            fullName2 += emitName;

            std::vector<TypeInfo> paramTypes2;
            paramTypes2.emplace_back(typeBare);
            for (auto& sp : dsig->params()) {
                TypeInfo pt = sp->type() ? sp->type()->getType() : TypeInfo();
                paramTypes2.push_back(pt.substitute(subst));
            }
            TypeInfo retType2 = dsig->retType() ? dsig->retType()->getType() : TypeInfo();
            retType2 = retType2.substitute(subst);

            SymbolInfo methodSym2(SymbolKind::Function, emitName, retType2);
            methodSym2.moduleName = implFile->moduleName();
            implFile->registerSymbol(fullName2, methodSym2);
            FnSymbolInfo methodFnSym2{fullName2, implFile->moduleName(), paramTypes2, retType2};
            methodFnSym2.isNoReturn = dsig->hasAnno("NoReturn");
            methodFnSym2.isConst = dsig->hasAnno("Const");
            methodFnSym2.fallibleErrType = dsig->resolvedFallibleErr();
            implFile->registerFnSymbol(fullName2, methodFnSym2);

            impl->addSpecDisambigEmit({.spec = spec, .sigIdx = sigIdx, .subst = subst, .emitMethodName = emitName});
        }
    }
}

bool SpecImplChecker::sigEquivalent(FnHeaderNode* implMethod, FnHeaderNode* specSig,
                                    const std::map<std::string, TypeInfo>& subst) const {

    auto implParams = implMethod->params();
    auto specParams = specSig->params();
    if (implParams.size() != specParams.size()) return false;

    for (size_t i = 0; i < implParams.size(); ++i) {
        TypeInfo ip = implParams[i]->type() ? implParams[i]->type()->getType() : TypeInfo();
        TypeInfo dp = specParams[i]->type() ? specParams[i]->type()->getType() : TypeInfo();
        TypeInfo dpSub = dp.substitute(subst);
        if (!(ip == dpSub)) return false;
    }

    TypeInfo ir = implMethod->retType() ? implMethod->retType()->getType() : TypeInfo();
    TypeInfo dr = specSig->retType() ? specSig->retType()->getType() : TypeInfo();
    TypeInfo drSub = dr.substitute(subst);
    if (ir != drSub) return false;
    return implMethod->resolvedFallibleErr() == specSig->resolvedFallibleErr();
}

bool SpecImplChecker::typeSatisfiesSpec(const std::string& typeBareName, SpecDeclNode* draft,
                                        const std::vector<TypeInfo>& specTypeArgs) const {
    if (!draft || !_yux) return false;

    // draft 自身泛型形参 → 实参替换表; arity 不齐时只覆盖前缀.
    std::map<std::string, TypeInfo> subst;
    const auto& dParams = draft->typeParams();
    size_t n = std::min(dParams.size(), specTypeArgs.size());
    for (size_t i = 0; i < n; ++i) {
        subst[dParams[i]] = specTypeArgs[i];
    }

    // 收集该类型在所有文件中的所有方法 (普通方法块 + draft impl 块都计入).
    std::vector<FnHeaderNode*> methods;
    auto collect = [&](FileNode* file) {
        if (!file) return;
        for (auto& impl : file->getStructImpls()) {
            if (impl->structName() != typeBareName) continue;
            for (auto& m : impl->methods()) {
                methods.push_back(m->header());
            }
        }
    };
    if (auto sdk = _yux->sdkFile()) collect(sdk);
    for (auto& f : _yux->files())
        collect(f);

    // 每个 draft 签名都要在 methods 中找到 §12.3.1 等价匹配 (受 subst 替换后).
    for (auto& dsig : draft->signatures()) {
        // 同 validateImpl: name() 按值返回临时 Token, getText() 是其成员引用,
        // 必须 copy 否则悬挂.
        const std::string dname = dsig->name().getText();
        bool hit = false;
        for (auto* m : methods) {
            if (m->name().getText() != dname) continue;
            if (sigEquivalent(m, dsig, subst)) {
                hit = true;
                break;
            }
        }
        if (!hit) return false;
    }
    return true;
}

bool SpecImplChecker::boundSatisfied(const TypeInfo& typeArg, SpecDeclNode* draft, const std::string& specQualified,
                                     const std::vector<TypeInfo>& specTypeArgs) const {
    if (!draft) return false;

    // §8.6.7.1: T 形参实参不接 `T&`. 这里只做正常形态; 调用侧若传入 ref,
    // 视作不满足任何 draft (上层 §6.4 应已拒绝).
    if (typeArg.isRef()) return false;

    const std::string& typeBare = typeArg.name;
    const std::string typeOwnerMod = moduleOfType(typeBare);
    const std::string typeQualified = typeOwnerMod.empty() ? typeBare : (typeOwnerMod + "." + typeBare);

    // 拼 specKey: 与 validateImpl 写入 _seen 时一致.
    std::string specKey = specQualified;
    if (!specTypeArgs.empty()) {
        specKey += "<";
        for (size_t i = 0; i < specTypeArgs.size(); ++i) {
            if (i) specKey += ",";
            specKey += specTypeArgs[i].getFullName();
        }
        specKey += ">";
    }

    if (_seen.find({typeQualified, specKey}) != _seen.end()) {
        return true;
    }

    if (draft->isDraftLike()) {
        return typeSatisfiesSpec(typeBare, draft, specTypeArgs);
    }
    return false;
}

// §12.9 / DRAFT-dyn-draft §4 对象安全 — Phase 2a.
//
// 递归扫描 draft 每个 fnSig 的所有参数类型 / 返回类型, 命中以下任一即不安全:
//   - 类型字面量名 == "Self"  (yux 当前无 Self 关键字, 但保留语义层禁用)
//   - 类型字面量名 == draft 自身名 (例如 `draft D { fn clone() D }`)
//
// "非 receiver" 位: yux fnSig 不显式承载 receiver, 故所有 params + retType
// 都需扫描. 结果按 draft 节点指针 memoize, 避免对同一 draft 重复遍历.
//
// 命中 false 时由 Phase 2b/2c 调用方翻为 E1134.
bool SpecImplChecker::specIsObjectSafe(SpecDeclNode* draft) const {
    if (!draft) return false;
    auto cached = _objectSafeCache.find(draft);
    if (cached != _objectSafeCache.end()) return cached->second;

    const std::string specName = draft->name().getText();

    // 直接遍历 TypeNode AST. 不用 TypeInfo: 在 draft 体内, TypeSelfNode 的
    // enclosing struct 名为空 (draft 不是 struct impl), TypeInfo.name 会丢
    // 成空串, 导致 Self 命中失败 (DRAFT-dyn-draft §4 漏判).
    std::function<bool(TypeNode*)> containsBad = [&](TypeNode* tn) -> bool {
        if (!tn) return false;
        if (dynamic_cast<TypeSelfNode*>(tn)) return true;
        if (auto* nn = dynamic_cast<TypeNormalNode*>(tn)) {
            return nn->typeNameToken().getText() == specName;
        }
        if (auto* gn = dynamic_cast<TypeGenericNode*>(tn)) {
            if (gn->baseName().getText() == specName) return true;
            for (auto& a : gn->typeArgs()) {
                if (containsBad(a)) return true;
            }
            return false;
        }
        if (auto* arr = dynamic_cast<TypeArrayNode*>(tn)) {
            return containsBad(arr->elementType());
        }
        if (auto* fn = dynamic_cast<TypeFnNode*>(tn)) {
            for (auto& pt : fn->paramTypes()) {
                if (containsBad(pt)) return true;
            }
            return containsBad(fn->retType());
        }
        if (auto* tup = dynamic_cast<TypeTupleNode*>(tn)) {
            for (auto& e : tup->elementTypes()) {
                if (containsBad(e)) return true;
            }
            return false;
        }
        return false;
    };

    bool safe = true;
    for (auto& sig : draft->signatures()) {
        for (auto& p : sig->params()) {
            if (containsBad(p->type())) {
                safe = false;
                break;
            }
        }
        if (!safe) break;
        if (containsBad(sig->retType())) {
            safe = false;
            break;
        }
    }
    _objectSafeCache[draft] = safe;
    return safe;
}

// §12.9 / DRAFT-dyn-draft Phase 2c: 类型声明位 Dyn 用法静态检查.
//
// 遍历入口: SDK + 用户文件; 对每个文件抓取所有"声明位"的 TypeNode 树:
//   - free fn / impl 方法 / 析构方法 / draft sig 的 params + retType
//   - struct field 类型
//   - enum variant payload 类型
//   - 顶层类型别名 target
// 每棵 TypeNode 树调 validateDynInTypeNode 递归校验. 命中即抛, 不收集多错.
//
// 局部 var 声明位 (fn 体内 declareAssign) 暂不在本 pass 覆盖范围: 形态上
// 必有初值 (typeWithRef 路径不允许无初值声明 Dyn<D&>; Dyn<D> 走声明位
// declareAssign 也必然有 Dyn:<D>(x) 初值), 命中后由 compileDynCtorExpr 的
// 构造检查 (Phase 2b) 接管 E1131..E1134.
void SpecImplChecker::validateDynTypeReferences() {
    if (!_yux) return;
    _dynAliasVisited.clear();
    // 确保 registry 已建好 (validate() 已 buildFromAllFiles 过, 但本方法
    // 可能在其它入口被独立调用, 这里再触发一次幂等).
    (void)_yux->specRegistry();

    auto walkFn = [&](FileNode* file, FnHeaderNode* hdr) {
        if (!hdr) return;
        for (auto& param : hdr->params()) {
            if (param && param->type()) {
                validateDynInTypeNode(param->type(), file, std::string());
            }
        }
        if (hdr->retType()) {
            validateDynInTypeNode(hdr->retType(), file, std::string());
        }
    };

    auto run = [&](FileNode* file) {
        if (!file) return;
        // free fn
        for (auto& fn : file->getFunctions()) {
            if (fn) walkFn(file, fn->header());
        }
        // struct field
        for (auto& sd : file->getStructDecls()) {
            if (!sd) continue;
            for (auto& f : sd->fields()) {
                if (f && f->type()) {
                    validateDynInTypeNode(f->type(), file, std::string());
                }
            }
        }
        // struct impl methods (含析构)
        for (auto& impl : file->getStructImpls()) {
            if (!impl) continue;
            for (auto& m : impl->methods()) {
                if (m) walkFn(file, m->header());
            }
            if (impl->destructor()) walkFn(file, impl->destructor()->header());
        }
        // draft sig
        for (auto& dd : file->getSpecDecls()) {
            if (!dd) continue;
            for (auto& sig : dd->signatures()) {
                walkFn(file, sig);
            }
        }
        // enum variant payload
        for (auto& ed : file->getEnumDecls()) {
            if (!ed) continue;
            for (auto& v : ed->variants()) {
                if (!v) continue;
                for (auto& pt : v->payloadTypes()) {
                    if (pt) validateDynInTypeNode(pt, file, std::string());
                }
            }
        }
        // 顶层类型别名 target
        for (auto& al : file->getAliasDecls()) {
            if (al && al->target()) {
                validateDynInTypeNode(al->target(), file, std::string());
            }
        }
    };
    // 去重：SDK flat 文件可能同时出现在 sdk->wildcardImports() 和 _yux->files() 中
    set<FileNode*> processedSet;
    auto runOnce = [&](FileNode* file) {
        if (!file || !processedSet.insert(file).second) return;
        run(file);
    };
    if (auto sdk = _yux->sdkFile()) {
        runOnce(sdk);
        for (auto* imp : sdk->wildcardImports()) {
            runOnce(imp);
        }
    }
    for (auto& f : _yux->files())
        runOnce(f);
}

void SpecImplChecker::validateDynInTypeNode(TypeNode* tn, FileNode* file, const std::string& outerWrapper) const {
    if (!tn) return;

    // TypeGenericNode: 处理 Dyn / 容器 / 通用递归
    if (auto* gen = dynamic_cast<TypeGenericNode*>(tn)) {
        const TypeInfo t = gen->getType();
        int line = tn->getLineNumber();
        int col = tn->getColumn();

        // 命中 Dyn 形态: 先按外层 wrapper 判 E1132 / E1135
        if (t.isDyn() && gen->typeArgs().size() == 1) {
            // 外层禁忌: Rc<Dyn> / Weak<Dyn> / Dyn<Dyn> → E1132;
            // Nullable<Dyn> (即 Dyn<D>?) → E1135.
            if (outerWrapper == "Rc" || outerWrapper == "Weak" || outerWrapper == "Dyn") {
                throw YuxError(line, col, ErrorCode::E1132, outerWrapper + "<" + t.getFullName() + ">");
            }
            if (outerWrapper == "Nullable") {
                throw YuxError(line, col, ErrorCode::E1135);
            }

            // 取 Dyn 的内层裸 draft 名: 允许 Ref<D> (= D&) 的借用形态.
            TypeNode* inner = gen->typeArgs()[0];
            TypeNode* innerStripped = inner;
            if (auto* refGen = dynamic_cast<TypeGenericNode*>(inner)) {
                if (refGen->getType().isRef() && refGen->typeArgs().size() == 1) {
                    innerStripped = refGen->typeArgs()[0];
                }
            }

            // 内层若仍是 Dyn → E1132
            if (auto* innerGen = dynamic_cast<TypeGenericNode*>(innerStripped)) {
                if (innerGen->getType().isDyn()) {
                    throw YuxError(line, col, ErrorCode::E1132, t.getFullName());
                }
            }

            // 内层必须是 TypeNormalNode(draft 名), 否则 E1131
            auto* innerNormal = dynamic_cast<TypeNormalNode*>(innerStripped);
            std::string specBare = innerNormal ? innerNormal->typeNameToken().getText() : std::string();
            SpecDeclNode* specDecl = nullptr;
            std::string specQualified;
            if (_yux && file && !specBare.empty()) {
                auto& reg = _yux->specRegistry();
                if (auto resolved = reg.resolve(specBare, file)) {
                    specDecl = resolved->decl;
                    specQualified = resolved->qualifiedName;
                }
            }
            if (!specDecl) {
                throw YuxError(line, col, ErrorCode::E1131, specBare.empty() ? std::string("?") : specBare);
            }

            // 对象安全 (E1134): 与构造侧一致, 声明位也拒绝 (DRAFT §4)
            if (!specIsObjectSafe(specDecl)) {
                throw YuxError(line, col, ErrorCode::E1134, specQualified, specQualified, specQualified);
            }

            // Dyn 内层为合法 draft, 不再继续递归 (D 名在 draft 命名空间, 不是
            // 一个会再嵌 Dyn 的类型). Ref 包裹下同理.
            return;
        }

        // 非 Dyn 容器: 决定下一层 wrapper 标签, 递归子项.
        // Array / Ref / Tuple / 用户结构体等不会触发包裹诊断; Rc/Weak/Nullable
        // 会传递给子项, 由子项的 Dyn 分支命中 E1132 / E1135.
        std::string childWrap;
        if (t.isRc() || t.isWeak() || t.isNullable()) {
            childWrap = t.name;
        }
        for (auto& arg : gen->typeArgs()) {
            if (arg) validateDynInTypeNode(arg, file, childWrap);
        }
        return;
    }

    // TypeArrayNode: 定长数组 [T*N], 元素类型继续递归
    if (auto* arr = dynamic_cast<TypeArrayNode*>(tn)) {
        if (arr->elementType()) {
            validateDynInTypeNode(arr->elementType(), file, std::string());
        }
        return;
    }

    // TypeFnNode: Function<P..., Ret>，参数 / 返回类型继续递归
    if (auto* fn = dynamic_cast<TypeFnNode*>(tn)) {
        for (auto& pt : fn->paramTypes()) {
            if (pt) validateDynInTypeNode(pt, file, std::string());
        }
        if (fn->retType()) {
            validateDynInTypeNode(fn->retType(), file, std::string());
        }
        return;
    }

    // TypeTupleNode: (T1, T2, ...), 各元素继续递归
    if (auto* tup = dynamic_cast<TypeTupleNode*>(tn)) {
        for (auto& e : tup->elementTypes()) {
            if (e) validateDynInTypeNode(e, file, std::string());
        }
        return;
    }

    // TypeNormalNode: 可能是指向 Dyn / Rc<Dyn> 的透明别名（`D = Dyn<Greet>` 后 `Rc<D>`）
    if (auto* normal = dynamic_cast<TypeNormalNode*>(tn)) {
        if (outerWrapper != "Rc" && outerWrapper != "Weak" && outerWrapper != "Dyn" && outerWrapper != "Nullable") {
            return;
        }
        if (!file) return;
        const std::string name = normal->typeNameToken().getText();
        AliasDeclNode* alias = file->getAliasDecl(name);
        if (!alias && _yux && _yux->sdkFile() && _yux->sdkFile() != file) {
            alias = _yux->sdkFile()->getAliasDecl(name);
        }
        if (!alias || !alias->target() || alias->isGeneric()) return;
        if (!_dynAliasVisited.insert(name).second) return; // 环，留给 E2016
        try {
            validateDynInTypeNode(alias->target(), file, outerWrapper);
        } catch (YuxError& e) {
            // 诊断钉在使用点 `Rc<D>`，不要指到别名定义行
            if (tn->getLineNumber() > 0) e.setLineNumber(tn->getLineNumber());
            e.setColumn(tn->getColumn());
            _dynAliasVisited.erase(name);
            throw;
        }
        _dynAliasVisited.erase(name);
    }
}

std::string SpecImplChecker::specTypeArgsSuffix(const SpecRef& ref) {
    if (ref.typeArgs.empty()) return {};
    std::string s = "<";
    for (size_t i = 0; i < ref.typeArgs.size(); ++i) {
        if (i) s += ",";
        s += ref.typeArgs[i].getFullName();
    }
    s += ">";
    return s;
}
