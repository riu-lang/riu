// Copyright (c) 2026. Yin-Jinlong@github

#ifndef YUX_LANG_MANGLER_H
#define YUX_LANG_MANGLER_H

#include "types.h"

// Mangler 集中管理所有用户/SDK 源符号的 LLVM 名称生成。
// 命名规则（仅作用于 .yux 源文件中定义的符号；运行时辅助、Windows API、
// LLVM intrinsic 等保留各自字面名称）：
//
//   函数         mod_name(params)            私有：mod__name(params)
//   全局常量     mod_name                    私有：mod__name
//   结构体类型   mod#Struct
//   方法         mod#Struct_name(params)     私有：mod#Struct__name(params)
//   构造函数     mod#Struct(params)
//   析构函数     mod#Struct_~()
//
// 参数列表始终带括号；类型间以 "," 分隔；类型形式取自 TypeInfo::getFullName()。
class Mangler {
public:
    // 函数：mod_name(params)；私有则 mod__name(params)
    static string function(const string& module, const string& name,
                           const vector<TypeInfo>& params, bool isPrivate);

    // 方法：mod#Struct_name(params)；私有则 mod#Struct__name(params)
    static string method(const string& module, const string& structName,
                         const string& methodName, const vector<TypeInfo>& params,
                         bool isPrivate);

    // 构造：mod#Struct(params)
    static string ctor(const string& module, const string& structName,
                       const vector<TypeInfo>& params);

    // 析构：mod#Struct_~()
    static string dtor(const string& module, const string& structName);

    // 结构体类型：mod#Struct
    static string structType(const string& module, const string& structName);

    // 全局常量：mod_name；私有则 mod__name
    static string global(const string& module, const string& name, bool isPrivate);

private:
    static string paramList(const vector<TypeInfo>& params);
    static string modPrefix(const string& module);            // 含尾 "_"
    static string modStructPrefix(const string& module, const string& structName); // mod#Struct
    static string privInfix(bool isPrivate);                  // 私有为 "_"，否则空
};

#endif //YUX_LANG_MANGLER_H
