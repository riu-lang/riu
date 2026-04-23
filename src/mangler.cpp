// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#include "mangler.h"

string Mangler::paramList(const vector<TypeInfo>& params) {
    string s = "(";
    for (size_t i = 0; i < params.size(); ++i) {
        if (i > 0) s += ",";
        s += params[i].getGenericMangleName();
    }
    s += ")";
    return s;
}

string Mangler::modPrefix(const string& module) {
    if (module.empty()) return "";
    return module + "_";
}

string Mangler::modStructPrefix(const string& module, const string& structName) {
    if (module.empty()) return structName;
    return module + "#" + structName;
}

string Mangler::privInfix(bool isPrivate) {
    return isPrivate ? "_" : "";
}

string Mangler::function(const string& module, const string& name,
                         const vector<TypeInfo>& params, bool isPrivate) {
    // 私有符号源名通常已带前导 "_"；这里不再额外添加，
    // 由源名的前导下划线与模块分隔符 "_" 自然形成 "mod__name"
    return modPrefix(module) + name + paramList(params);
}

string Mangler::method(const string& module, const string& structName,
                       const string& methodName, const vector<TypeInfo>& params,
                       bool /*isPrivate*/) {
    // 同 function：私有方法的 "_" 来自源名前导下划线
    return modStructPrefix(module, structName) + "_" + methodName + paramList(params);
}

string Mangler::ctor(const string& module, const string& structName,
                     const vector<TypeInfo>& params) {
    return modStructPrefix(module, structName) + paramList(params);
}

string Mangler::dtor(const string& module, const string& structName) {
    return modStructPrefix(module, structName) + "_~()";
}

string Mangler::structType(const string& module, const string& structName) {
    return modStructPrefix(module, structName);
}

string Mangler::global(const string& module, const string& name, bool /*isPrivate*/) {
    return modPrefix(module) + name;
}
