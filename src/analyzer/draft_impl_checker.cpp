// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// draft 显式实现校验器 — 实现 (spec §12).
//
// 本文件实现:
//   - DraftImplChecker::validate(): 遍历 SDK + 用户文件中所有
//     带 draftRefs 的 StructImplNode, 逐一调 validateImpl.
//   - validateImpl: 解析每个 D 的 qualifiedName, 顺序做 orphan / 重复 /
//     穷尽匹配 / 不多余检查; 对多 D 实现块, 不多余检查在所有 D 都比对
//     完后再统一判定 (任一 D 命中即视为合法).
//   - sigEquivalent: §12.3.1 等价判定 — 名 / 形参类型 / 返回类型;
//     先对 draft 侧应用 typeParam → impl 块给出的类型实参替换.

#include "draft_impl_checker.h"

#include "draft_registry.h"
#include "ast/yux.h"

#include <set>
#include <tuple>
#include <vector>

namespace {

// 内置类型按 yux.core 归属 (§12.5 orphan 用; SDK base.yux 内为这些类型
// 实现 ToString 等内置 draft 视为合法).
const std::set<std::string>& builtinTypeNames() {
    static const std::set<std::string> s = {
        "i8", "u8", "i16", "u16",
        "i32", "u32", "i64", "u64",
        "f32", "f64", "bool",
        "String", "StringBuilder",
        "Box", "Array", "Weak", "Ptr",
        "Nullable", "Ref"
    };
    return s;
}

} // namespace

DraftImplChecker::DraftImplChecker(Yux* yux) : _yux(yux) {}

void DraftImplChecker::buildTypeOwnerMap() {
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
        for (auto& f : _yux->files()) index(f);
    }
}

std::string DraftImplChecker::moduleOfType(const std::string& bareName) const {
    auto it = _typeOwnerModule.find(bareName);
    if (it != _typeOwnerModule.end()) return it->second;
    if (builtinTypeNames().count(bareName)) return "yux.core";
    return {};
}

void DraftImplChecker::validate() {
    if (!_yux) return;
    buildTypeOwnerMap();
    // 触发一次 registry 构建 (若未构建)
    (void)_yux->draftRegistry();

    auto run = [&](FileNode* file) {
        if (!file) return;
        for (auto& impl : file->getStructImpls()) {
            // 普通方法块 (不带 `: D`) 不在本 checker 范围.
            if (impl->draftRefs().empty()) continue;
            validateImpl(file, impl);
        }
    };
    if (auto sdk = _yux->sdkFile()) run(sdk);
    for (auto& f : _yux->files()) run(f);

    // §12.4.2.1 E1105 显隐冲突: 必须等所有显式 impl 全部 §12.2 校验通过
    // 后再做, 避免"穷尽性 / 不多余" 与 显隐冲突 互相覆盖错误位置.
    checkExplicitImplicitConflict();
}

void DraftImplChecker::checkExplicitImplicitConflict() {
    // 每个 Type 收集普通方法块与 draft 实现块的方法; 配对比对签名等价.
    // 跨包条款 (§12.4.2.2 / §12.4.2.3) 已由 #DraftLike 路径与 §12.5
    // orphan 处理: 这里所见的"普通方法块 + draft 实现块共存"场景, 都是
    // §12.4.2.1 情形 A, 直接报 E1105.
    struct PerType {
        // (impl 块所在文件, 方法 header)
        std::vector<std::pair<FileNode*, FnHeaderNode*>> plain;
        // (impl 块所在文件, 方法 header, 该 impl 块首个 draftRef 的限定名 — 仅用于错误信息)
        std::vector<std::tuple<FileNode*, FnHeaderNode*, std::string>> draft;
    };
    std::map<std::string, PerType> table;

    auto draftQualifiedFor = [&](FileNode* file, StructImplNode* impl) -> std::string {
        if (impl->draftRefs().empty()) return {};
        auto& reg = _yux->draftRegistry();
        const auto& dref = impl->draftRefs().front();
        if (auto r = reg.resolve(dref.name, file)) return r->qualifiedName;
        return dref.name;
    };

    auto collect = [&](FileNode* file) {
        if (!file) return;
        for (auto& impl : file->getStructImpls()) {
            const std::string typeBare = impl->structName();
            const std::string ownerMod = moduleOfType(typeBare);
            const std::string typeQualified = ownerMod.empty()
                ? typeBare
                : (ownerMod + "." + typeBare);
            auto& slot = table[typeQualified];
            if (impl->draftRefs().empty()) {
                for (auto& m : impl->methods()) {
                    slot.plain.emplace_back(file, m->header());
                }
            } else {
                std::string dq = draftQualifiedFor(file, impl);
                for (auto& m : impl->methods()) {
                    slot.draft.emplace_back(file, m->header(), dq);
                }
            }
        }
    };
    if (auto sdk = _yux->sdkFile()) collect(sdk);
    for (auto& f : _yux->files()) collect(f);

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
                    throw YuxError(pm->getLineNumber(), pm->getColumn(),
                                   ErrorCode::E1105,
                                   pm->name().getText(), typeQualified, dq);
                }
            }
        }
    }
}

void DraftImplChecker::validateImpl(FileNode* implFile, StructImplNode* impl) {
    auto& reg = _yux->draftRegistry();
    const std::string typeBare = impl->structName();
    const std::string typeOwnerMod = moduleOfType(typeBare);
    const std::string typeQualified = typeOwnerMod.empty()
        ? typeBare
        : (typeOwnerMod + "." + typeBare);

    const auto& implMethods = impl->methods();

    // 多 D impl 块的"不多余"聚合命中表 (任一 D 命中即视为合法).
    std::vector<bool> aggMatched(implMethods.size(), false);

    for (auto& dref : impl->draftRefs()) {
        auto resolved = reg.resolve(dref.name, implFile);
        if (!resolved) {
            // §10 名字解析失败. 复用 E3032 (未来可专门给一个 E11xx 码).
            throw YuxError(dref.line, dref.col, ErrorCode::E3032, dref.name);
        }
        DraftDeclNode* draft = resolved->decl;
        const std::string& draftQualified = resolved->qualifiedName;
        const std::string draftKey = draftQualified + draftTypeArgsSuffix(dref);

        // §12.2.2.2 重复
        auto key = std::make_pair(typeQualified, draftKey);
        auto seenIt = _seen.find(key);
        if (seenIt != _seen.end()) {
            throw YuxError(impl->getLineNumber(), impl->getColumn(),
                           ErrorCode::E1103, typeQualified, draftKey);
        }
        _seen.emplace(key, impl);

        // §12.5 orphan: implMod ∈ {typeOwnerMod, draftOwnerMod} 之一即合法.
        const std::string draftOwnerMod = resolved->ownerFile
            ? resolved->ownerFile->moduleName()
            : std::string();
        const std::string& implMod = implFile->moduleName();
        // implMod 为空 (单文件主入口未登记模块名) 时, 跳过 orphan 检查避免误报.
        if (!implMod.empty()
            && implMod != typeOwnerMod
            && implMod != draftOwnerMod) {
            throw YuxError(impl->getLineNumber(), impl->getColumn(),
                           ErrorCode::E1120, draftQualified, typeQualified);
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

        // §12.2.2.1 穷尽性: 对每个 draft 签名, 必须在 impl 中匹配一个同名
        // + 等价签名的方法. 命中位置同步标记 aggMatched.
        for (auto& dsig : draft->signatures()) {
            // 注意: FnHeaderNode::name() 按值返回 Token, getText() 是它的成员引用;
            // 不能写成 `const std::string& dname = dsig->name().getText();` ——
            // 临时 Token 在 full-expression 后销毁, dname 立即悬挂.
            const std::string dname = dsig->name().getText();
            int hit = -1;
            for (size_t i = 0; i < implMethods.size(); ++i) {
                auto& m = implMethods[i]->header();
                if (m->name().getText() != dname) continue;
                if (sigEquivalent(m, dsig, subst)) {
                    hit = static_cast<int>(i);
                    break;
                }
            }
            if (hit < 0) {
                // E1101: "Type '{}' does not implement draft method '{}: {}'"
                std::string sigDesc = dname;
                throw YuxError(impl->getLineNumber(), impl->getColumn(),
                               ErrorCode::E1101, typeQualified,
                               draftQualified, sigDesc);
            }
            aggMatched[hit] = true;
        }
    }

    // §12.2.2.1 不多余: 任一 D 都未命中的 impl 方法报 E1102.
    // 析构 / 关联函数等辅助按 §7.8 走单独 fnClean / 普通方法块,
    // 不会落到 draft impl 块的 _methods 里, 故这里不需特殊放行.
    for (size_t i = 0; i < implMethods.size(); ++i) {
        if (!aggMatched[i]) {
            auto& m = implMethods[i]->header();
            throw YuxError(m->getLineNumber(), m->getColumn(),
                           ErrorCode::E1102, m->name().getText(), typeQualified);
        }
    }
}

bool DraftImplChecker::sigEquivalent(
    FnHeaderNode* implMethod,
    FnHeaderNode* draftSig,
    const std::map<std::string, TypeInfo>& subst) const {

    auto implParams = implMethod->params();
    auto draftParams = draftSig->params();
    if (implParams.size() != draftParams.size()) return false;

    for (size_t i = 0; i < implParams.size(); ++i) {
        TypeInfo ip = implParams[i]->type() ? implParams[i]->type()->getType() : TypeInfo();
        TypeInfo dp = draftParams[i]->type() ? draftParams[i]->type()->getType() : TypeInfo();
        TypeInfo dpSub = dp.substitute(subst);
        if (!(ip == dpSub)) return false;
    }

    TypeInfo ir = implMethod->retType() ? implMethod->retType()->getType() : TypeInfo();
    TypeInfo dr = draftSig->retType() ? draftSig->retType()->getType() : TypeInfo();
    TypeInfo drSub = dr.substitute(subst);
    return ir == drSub;
}

bool DraftImplChecker::typeSatisfiesDraft(
    const std::string& typeBareName,
    DraftDeclNode* draft,
    const std::vector<TypeInfo>& draftTypeArgs) const {
    if (!draft || !_yux) return false;

    // draft 自身泛型形参 → 实参替换表; arity 不齐时只覆盖前缀.
    std::map<std::string, TypeInfo> subst;
    const auto& dParams = draft->typeParams();
    size_t n = std::min(dParams.size(), draftTypeArgs.size());
    for (size_t i = 0; i < n; ++i) {
        subst[dParams[i]] = draftTypeArgs[i];
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
    for (auto& f : _yux->files()) collect(f);

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

bool DraftImplChecker::boundSatisfied(
    const TypeInfo& typeArg,
    DraftDeclNode* draft,
    const std::string& draftQualified,
    const std::vector<TypeInfo>& draftTypeArgs) const {
    if (!draft) return false;

    // §8.6.7.1: T 形参实参不接 `T&`. 这里只做正常形态; 调用侧若传入 ref,
    // 视作不满足任何 draft (上层 §6.4 应已拒绝).
    if (typeArg.isRef()) return false;

    const std::string& typeBare = typeArg.name;
    const std::string typeOwnerMod = moduleOfType(typeBare);
    const std::string typeQualified = typeOwnerMod.empty()
        ? typeBare
        : (typeOwnerMod + "." + typeBare);

    // 拼 draftKey: 与 validateImpl 写入 _seen 时一致.
    std::string draftKey = draftQualified;
    if (!draftTypeArgs.empty()) {
        draftKey += "<";
        for (size_t i = 0; i < draftTypeArgs.size(); ++i) {
            if (i) draftKey += ",";
            draftKey += draftTypeArgs[i].getFullName();
        }
        draftKey += ">";
    }

    if (_seen.find({typeQualified, draftKey}) != _seen.end()) {
        return true;
    }

    if (draft->isDraftLike()) {
        return typeSatisfiesDraft(typeBare, draft, draftTypeArgs);
    }
    return false;
}

std::string DraftImplChecker::draftTypeArgsSuffix(const DraftRef& ref) {
    if (ref.typeArgs.empty()) return {};
    std::string s = "<";
    for (size_t i = 0; i < ref.typeArgs.size(); ++i) {
        if (i) s += ",";
        s += ref.typeArgs[i].getFullName();
    }
    s += ">";
    return s;
}
