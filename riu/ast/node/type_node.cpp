// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#include "type_node.h"

#include "ast/name_lookup.h"
#include "ast/type_path.h"
#include "file_node.h"

const TypeInfo& TypeSelfNode::getType() const {
    if (_cachedType) return *_cachedType;
    // structName 为空 (typically spec 体内): 返回名为 "Self" 的占位 TypeInfo,
    // 由 SpecImplChecker::sigEquivalent 通过 subst["Self"] 替换为 impl 具体类型;
    // 默认体 fall-through 编译时由 Compiler::compileInheritedDefaults 临时
    // setStructName 走真实 codegen.
    if (_structName.empty()) return cacheType(TypeInfo("Self"));
    TypeInfo t(_structName);
    if (!_ownerModule.empty()) {
        t.ownerModule = _ownerModule;
    } else if (auto* f = enclosingFile()) {
        t.ownerModule = f->moduleName();
    }
    return cacheType(std::move(t));
}

const TypeInfo& TypeNormalNode::getType() const {
    if (_cachedType) return *_cachedType;

    // 块 / struct 内 `type` 立即展开；文件顶层别名仍走 resolveTypePath + resolveAlias。
    if (_path.isBare()) {
        const string& last = _path.last().getText();
        // 标量不能被 `type i32 = ...` 挡住：混测几乎全是 i32/bool，跳过 parent 链
        // dynamic_cast。其它语言名仍先查局部别名。
        if (!isBuiltinType(last)) {
            if (auto* a = sema::lookupScopedAlias(this, last)) {
                return cacheType(sema::expandScopedAlias(a));
            }
        }
        if (isLanguageNamedType(last)) {
            _cachedType = &internNamedType(last);
            return *_cachedType;
        }
    }
    // 不 loadModule：resolveTypePath 只走已 use/load 的别名与 packageChild
    auto r = resolveTypePath(enclosingFile(), nullptr, _path, getLineNumber(), getColumn());
    return cacheType(std::move(r.type));
}

const TypeInfo& TypeGenericNode::getType() const {
    if (_cachedType) return *_cachedType;
    auto r = resolveTypePath(enclosingFile(), nullptr, _path, getLineNumber(), getColumn());
    vector<sp<TypeInfo>> args;
    args.reserve(_typeArgs.size());
    for (auto& typeArg : _typeArgs) {
        args.push_back(internTypeSpAt(this, typeArg->getType()));
    }
    TypeInfo t{r.type.name, std::move(args)};
    if (t.kind == TypeKind::Generic) t.ownerModule = r.type.ownerModule;
    return cacheType(std::move(t));
}
