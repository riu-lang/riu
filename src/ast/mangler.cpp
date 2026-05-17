// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

#include "mangler.h"

// 命名约定（详见 mangler.h）：
//   函数         mod_fn(types)            私有：mod__fn(types)
//   方法         mod#Struct_m(types)      私有：mod#Struct__m(types)
//   构造         mod#Struct(types)
//   析构         mod#Struct_~()
//   结构体       mod#Struct
//   全局常量     mod_name                 私有：mod__name
// 运行时辅助、Windows API、LLVM intrinsic 保留各自字面名称（不参与 mangling）。
// 其他模块只声明为 external，链接时引用 yux.obj 中的实现。

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

string Mangler::staticMethod(const string& module, const string& structName,
                             const string& methodName, const vector<TypeInfo>& params) {
    // DRAFT-static-fn: `mod#Struct::name(params)`. `::` 分隔避免与实例方法
    // `mod#Struct_name(params)` 同名冲突, 同时与源码调用语法对齐 (Type::name).
    return modStructPrefix(module, structName) + "::" + methodName + paramList(params);
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

string Mangler::lambda(const string& module, int line, int col) {
    // 模块名内的 '.' 在 LLVM 符号里没问题，但与 Mangler 其余分隔符的视觉风格不一致
    // 这里同步替换成 '_'，得到形如 __lambda_yux_core_string_42_7 的名字
    string sanitized;
    sanitized.reserve(module.size());
    for (char c : module) {
        sanitized += (c == '.') ? '_' : c;
    }
    if (sanitized.empty()) sanitized = "anon";
    return "__lambda_" + sanitized + "_" + std::to_string(line) + "_" + std::to_string(col);
}
