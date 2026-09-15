// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#include "generic.h"

#include "ast/name_lookup.h"

string generic::SubstStack::formatInstantiationContext() const {
    if (_frames.empty()) return "";
    string result;
    for (auto it = _frames.rbegin(); it != _frames.rend(); ++it) {
        const auto& frame = *it;
        if (!frame.effStructName.empty()) {
            if (!result.empty()) result += "\n  ";
            result += "instantiated as '" + frame.effStructName + "'";
            if (!frame.sourceFile.empty()) {
                result += " at " + frame.sourceFile;
                if (frame.sourceLine > 0) {
                    result += ":" + to_string(frame.sourceLine);
                }
            }
        }
    }
    return result;
}

bool generic::bindsStructSelf(const TypeInfo& t, const string& baseName, const string& effName) {
    if (effName.empty()) return false;
    return t.isSelf() || (t.kind == TypeKind::Normal && !baseName.empty() && t.name == baseName);
}

TypeInfo generic::bindStructSelfType(const TypeInfo& t, const string& baseName, const string& effName,
                                     const TypeInfo& effType) {
    return bindsStructSelf(t, baseName, effName) ? effType : t;
}

TypeInfo generic::applySubst(const TypeInfo& t, const SubstStack& stack, const string& currentStructName,
                             FileNode* file, FileNode* sdkFile, NamedStructFn namedStruct, const void* namedCtx) {
    TypeInfo result = t;
    if (!stack.empty()) {
        // 从栈底到栈顶依次 substitute：实例方法体内若再压一帧泛型 fn（baseStructName
        // 为空），只看顶帧会丢掉 struct 单态的 T→concrete，`Self` / 裸 `Slot` 也绑不上。
        for (const auto& frame : stack) {
            result = result.substitute(frame.subst);
        }
        for (auto it = stack.rbegin(); it != stack.rend(); ++it) {
            if (!bindsStructSelf(result, it->baseStructName, it->effStructName)) continue;
            TypeInfo bound = namedStruct(namedCtx, it->effStructName);
            if (bound.kind != result.kind || bound.name != result.name) {
                result = std::move(bound);
                break;
            }
        }
    } else if (result.isSelf() && !currentStructName.empty()) {
        result = namedStruct(namedCtx, currentStructName);
    }
    return sema::resolveAlias(result, file, sdkFile);
}

TypeInfo generic::applySubstMap(const TypeInfo& t, const map<string, TypeInfo>* subst) {
    if (!subst || subst->empty()) return t;
    return t.substitute(*subst);
}
