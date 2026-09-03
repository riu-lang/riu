// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#include "mangler.h"

// 命名约定（详见 mangler.h）：
//   函数         mod.name(params)           私有：mod._name(params)
//   方法         mod.Struct.method(params)   私有：mod.Struct._method(params)
//   析构         mod.Struct::~()
//   结构体       mod.Struct
//   全局常量     mod.name                   私有：mod._name
// 运行时辅助、Windows API、LLVM intrinsic 保留各自字面名称（不参与 mangling）。
// 其他模块只声明为 external，链接时引用 yux.obj 中的实现。

vector<TypeInfo> Mangler::fnSignatureTypes(const vector<TypeInfo>& params, const TypeInfo& retType,
                                           const string& fallibleErrType) {
    vector<TypeInfo> sig = params;
    if (!fallibleErrType.empty()) {
        TypeInfo slot;
        if (retType.empty()) {
            slot.attachFallibleErr(fallibleErrType);
        } else {
            slot = retType;
            slot.attachFallibleErr(fallibleErrType);
        }
        sig.push_back(slot);
    }
    return sig;
}

string Mangler::paramList(const vector<TypeInfo>& params, const TypeInfo& retType, const string& fallibleErrType) {
    const auto sig = fnSignatureTypes(params, retType, fallibleErrType);
    string s = "(";
    for (size_t i = 0; i < sig.size(); ++i) {
        if (i > 0) s += ",";
        s += sig[i].getMangleName();
    }
    s += ")";
    return s;
}

string Mangler::modPrefix(const string& module) {
    if (module.empty()) return "";
    return module + ".";
}

string Mangler::modStructPrefix(const string& module, const string& structName) {
    if (module.empty()) return structName;
    return module + "." + structName;
}

string Mangler::function(const string& module, const string& name, const vector<TypeInfo>& params, bool /*isPrivate*/,
                         const TypeInfo& retType, const string& fallibleErrType) {
    // 私有符号源名已带前导 "_"（如 _foo），与模块 "." 自然形成 mod._foo
    return modPrefix(module) + name + paramList(params, retType, fallibleErrType);
}

string Mangler::method(const string& module, const string& structName, const string& methodName,
                       const vector<TypeInfo>& params, bool /*isPrivate*/, const TypeInfo& retType,
                       const string& fallibleErrType) {
    // 私有方法的 "_" 来自源名前导下划线（如 _helper），与 "." 自然形成 Struct._helper
    return modStructPrefix(module, structName) + "." + methodName + paramList(params, retType, fallibleErrType);
}

string Mangler::staticMethod(const string& module, const string& structName, const string& methodName,
                             const vector<TypeInfo>& params, const TypeInfo& retType, const string& fallibleErrType) {
    // DRAFT-static-fn: `mod.Struct::name(params)`. `::` 分隔避免与实例方法
    // `mod.Struct.name(params)` 同名冲突, 同时与源码调用语法对齐 (Type::name).
    return modStructPrefix(module, structName) + "::" + methodName + paramList(params, retType, fallibleErrType);
}

string Mangler::staticField(const string& module, const string& structName, const string& fieldName) {
    // DRAFT-static-vars Phase 4: mod.Struct::FIELD
    return modStructPrefix(module, structName) + "::" + fieldName;
}

string Mangler::dtor(const string& module, const string& structName) {
    return modStructPrefix(module, structName) + "::~()";
}

string Mangler::structType(const string& module, const string& structName) {
    return modPrefix(module) + structName;
}

string Mangler::global(const string& module, const string& name, bool /*isPrivate*/) {
    return modPrefix(module) + name;
}

string Mangler::lambda(const string& module, int line, int col) {
    return modPrefix(module) + "__lambda_" + std::to_string(line) + "_" + std::to_string(col);
}
