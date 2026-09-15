// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#include "generic.h"

#include "ast/name_lookup.h"
#include "ast/node/file_node.h"
#include "ast/node/struct_node.h"

string generic::StructInstance::ownerModule() const {
    if (ownerFile) return ownerFile->moduleName();
    return consumerModule;
}

TypeInfo generic::StructInstance::typeInfo() const {
    if (!baseDecl) return {};
    vector<sp<TypeInfo>> spArgs;
    spArgs.reserve(args.size());
    for (const auto& a : args) {
        spArgs.push_back(std::make_shared<TypeInfo>(a));
    }
    TypeInfo t{baseDecl->name().getText(), std::move(spArgs)};
    if (ownerFile) t.ownerModule = ownerFile->moduleName();
    return t;
}

map<string, TypeInfo> generic::StructInstance::substMap() const {
    map<string, TypeInfo> subst;
    if (!baseDecl) return subst;
    const auto& tparams = baseDecl->typeParams();
    for (size_t i = 0; i < tparams.size() && i < args.size(); ++i) {
        subst[tparams[i]] = args[i];
    }
    return subst;
}

generic::StructInstance generic::makeStructInstance(StructDeclNode* baseDecl, vector<TypeInfo> args,
                                                    FileNode* ownerFile, FileNode* currentFile, FileNode* sdkFile,
                                                    string mangledName, int sourceLine) {
    StructInstance inst;
    inst.baseDecl = baseDecl;
    inst.ownerFile = ownerFile ? ownerFile : currentFile;
    inst.mangledName = std::move(mangledName);
    string baseName = baseDecl->name().getText();
    inst.baseImpl = inst.ownerFile ? inst.ownerFile->getStructImpl(baseName) : nullptr;
    if (!inst.baseImpl && sdkFile && sdkFile != inst.ownerFile) {
        inst.baseImpl = sdkFile->getStructImpl(baseName);
    }
    inst.args = std::move(args);
    inst.sourceFile = currentFile ? currentFile->moduleName() : "";
    inst.sourceLine = sourceLine;
    inst.consumerModule =
        inst.ownerFile ? inst.ownerFile->moduleName() : (currentFile ? currentFile->moduleName() : "");
    return inst;
}

generic::StructInstance* generic::StructTable::find(const string& mangledName) {
    auto it = _instances.find(mangledName);
    return it == _instances.end() ? nullptr : &it->second;
}

const generic::StructInstance* generic::StructTable::find(const string& mangledName) const {
    auto it = _instances.find(mangledName);
    return it == _instances.end() ? nullptr : &it->second;
}

void generic::StructTable::insert(StructInstance inst) {
    string key = inst.mangledName;
    _instances.emplace(std::move(key), std::move(inst));
}

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
