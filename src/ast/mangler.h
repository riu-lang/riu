// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

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

    // 静态方法 (DRAFT-static-fn)：mod#Struct::name(params)
    // 与 instance method (Struct_name) 用 `::` 区分; 无 receiver, 与实例同名不冲突
    static string staticMethod(const string& module, const string& structName,
                               const string& methodName, const vector<TypeInfo>& params);

    // 静态字段 (DRAFT-static-vars Phase 4)：mod#Struct::FIELD
    // 与 staticMethod 同用 `::` 分隔，与实例字段 mod#Struct.FIELD 不冲突
    static string staticField(const string& module, const string& structName,
                              const string& fieldName);

    // 析构：mod#Struct_~()
    static string dtor(const string& module, const string& structName);

    // 结构体类型：mod#Struct
    static string structType(const string& module, const string& structName);

    // 全局常量：mod_name；私有则 mod__name
    static string global(const string& module, const string& name, bool isPrivate);

    // Lambda 顶层匿名 fn：__lambda_<sanitized-mod>_<line>_<col>
    // module 中的 '.' 替换为 '_'，避免与 Mangler 其他分隔符冲突
    static string lambda(const string& module, int line, int col);

private:
    static string paramList(const vector<TypeInfo>& params);
    static string modPrefix(const string& module);            // 含尾 "_"
    static string modStructPrefix(const string& module, const string& structName); // mod#Struct
    static string privInfix(bool isPrivate);                  // 私有为 "_"，否则空
};

#endif //YUX_LANG_MANGLER_H
