// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#include "type_node.h"

#include "ast/type_path.h"
#include "file_node.h"

TypeInfo TypeSelfNode::getType() const {
    // structName 为空 (typically spec 体内): 返回名为 "Self" 的占位 TypeInfo,
    // 由 SpecImplChecker::sigEquivalent 通过 subst["Self"] 替换为 impl 具体类型;
    // 默认体 fall-through 编译时由 Compiler::compileInheritedDefaults 临时
    // setStructName 走真实 codegen.
    if (_structName.empty()) return TypeInfo("Self");
    TypeInfo t(_structName);
    if (!_ownerModule.empty()) {
        t.ownerModule = _ownerModule;
    } else if (auto* f = enclosingFile()) {
        t.ownerModule = f->moduleName();
    }
    return t;
}

TypeInfo TypeNormalNode::getType() const {
    // 不 loadModule：resolveTypePath 只走已 use/load 的别名与 packageChild
    auto r = resolveTypePath(enclosingFile(), nullptr, _path, getLineNumber(), getColumn());
    return r.type;
}

TypeInfo TypeGenericNode::getType() const {
    auto r = resolveTypePath(enclosingFile(), nullptr, _path, getLineNumber(), getColumn());
    vector<sp<TypeInfo>> args;
    args.reserve(_typeArgs.size());
    for (auto& typeArg : _typeArgs) {
        args.push_back(make_shared<TypeInfo>(typeArg->getType()));
    }
    TypeInfo t{r.type.name, std::move(args)};
    if (t.kind == TypeKind::Generic) t.ownerModule = r.type.ownerModule;
    return t;
}
