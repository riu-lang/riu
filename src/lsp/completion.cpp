// Copyright (c) 2026. Yin-Jinlong@github
// MPL-2.0

// LSP 静态补全表实现
// 列表来源：yux-vscode/src/extension.ts 中的 KEYWORDS / TYPES / BUILTIN_FUNCTIONS
// / TYPE_CONVERSION_FUNCTIONS / SNIPPETS。修改时请保持两侧一致；本表为
// LSP 切换后的单一事实源，TS 那份在 P3 删除。

#include "completion.h"

namespace yux::lsp {

namespace {

const std::vector<CompletionItem>& buildItems() {
    static const std::vector<CompletionItem> items = [] {
        std::vector<CompletionItem> v;
        auto kw = [&](std::string label, std::string detail) {
            v.push_back({std::move(label), CompletionKind::Keyword, std::move(detail), {}, InsertFormat::PlainText, {}});
        };
        auto ty = [&](std::string label, std::string detail) {
            v.push_back({std::move(label), CompletionKind::Class, std::move(detail), {}, InsertFormat::PlainText, {}});
        };
        auto fn = [&](std::string label, std::string detail) {
            v.push_back({std::move(label), CompletionKind::Function, std::move(detail), {}, InsertFormat::PlainText, {}});
        };
        auto sn = [&](std::string label, std::string detail, std::string insert, std::string doc) {
            v.push_back({std::move(label), CompletionKind::Snippet, std::move(detail), std::move(insert), InsertFormat::Snippet, std::move(doc)});
        };

        // KEYWORDS
        kw("fn", "函数声明");
        kw("ret", "返回语句");
        kw("if", "条件语句");
        kw("elif", "else if 条件");
        kw("else", "else 分支");
        kw("match", "模式匹配");
        kw("true", "布尔真值");
        kw("false", "布尔假值");
        kw("null", "空值");
        kw("let", "变量声明（默认浅不可变；加 #Mut 可变）");
        kw("struct", "结构体声明");
        kw("enum", "枚举声明");
        kw("draft", "约束/接口声明");
        kw("loop", "循环语句");
        kw("break", "跳出循环");
        kw("use", "导入模块");
        kw("extern", "外部声明");
        kw("try", "try 块（错误处理）");
        kw("catch", "catch 分支（错误处理）");

        // TYPES
        ty("i8",  "8位有符号整数");
        ty("i16", "16位有符号整数");
        ty("i32", "32位有符号整数");
        ty("i64", "64位有符号整数");
        ty("u8",  "8位无符号整数");
        ty("u16", "16位无符号整数");
        ty("u32", "32位无符号整数");
        ty("u64", "64位无符号整数");
        ty("f32", "32位浮点数");
        ty("f64", "64位浮点数");
        ty("bool", "布尔类型");
        ty("String", "字符串类型");
        ty("Ref",   "引用类型 Ref<T>");
        ty("Rc",   "堆对象类型 Rc<T>");
        ty("Ptr",   "原始指针类型 Ptr<T>");
        ty("Array", "动态数组类型 Array<T>");

        // BUILTIN_FUNCTIONS
        fn("print",   "内置函数：打印输出");
        fn("println", "内置函数：打印输出并换行");

        // TYPE_CONVERSION_FUNCTIONS
        for (const char* t : {"bool","i8","i16","i32","i64","u8","u16","u32","u64","f32","f64"}) {
            std::string label = "to_";
            label += t;
            std::string detail = "类型转换：转换为 ";
            detail += t;
            fn(std::move(label), std::move(detail));
        }

        // SNIPPETS
        sn("fn", "函数声明模板",
           "fn ${1:name}(${2:params}) ${3:retType} {\n\t${4:body}\n}",
           "创建一个新函数");
        sn("fn-expr", "函数表达式模板",
           "fn ${1:name}(${2:params}) = ${3:expr}",
           "创建一个表达式函数");
        sn("if", "if 条件模板",
           "if ${1:condition} {\n\t${2:body}\n}",
           "创建一个 if 条件语句");
        sn("if-else", "if-else 条件模板",
           "if ${1:condition} {\n\t${2:body}\n} else {\n\t${3:elseBody}\n}",
           "创建一个 if-else 条件语句");
        sn("if-elif-else", "if-elif-else 条件模板",
           "if ${1:condition} {\n\t${2:body}\n} elif ${3:condition2} {\n\t${4:body2}\n} else {\n\t${5:elseBody}\n}",
           "创建一个完整的 if-elif-else 条件语句");
        sn("struct", "结构体声明模板",
           "struct ${1:Name} {\n\t${2:field} ${3:type}\n}",
           "创建一个结构体声明");
        sn("impl", "结构体实现模板",
           "${1:Name} {\n\tfn ${2:method}(${3:params}) {\n\t\t${4:body}\n\t}\n}",
           "创建一个结构体实现块");
        sn("draft", "draft 声明模板",
           "draft ${1:Name} {\n\tfn ${2:method}(${3:params}) ${4:retType}\n}",
           "创建一个 draft 声明");
        sn("draft-impl", "draft 实现块模板",
           "${1:Type} : ${2:Draft} {\n\tfn ${3:method}(${4:params}) ${5:retType} {\n\t\t${6:body}\n\t}\n}",
           "创建一个 draft 实现块");
        sn("let", "变量声明模板（默认浅不可变）",
           "let ${1:name} ${2:type} = ${3:value}",
           "创建一个不可变变量声明（默认 let）");
        sn("let-mut", "可变变量声明模板",
           "#Mut let ${1:name} ${2:type} = ${3:value}",
           "创建一个可变变量声明（#Mut let）");
        sn("let-cval", "编译时常量声明模板",
           "#Cval let ${1:name} ${2:type} = ${3:value}",
           "创建一个编译时常量声明（#Cval let）");
        sn("let-global", "全局常量声明模板",
           "#Cval\nlet ${1:NAME} ${2:type} = ${3:value}",
           "创建一个全局常量（全局 let 必须 #Cval）");
        sn("ret", "返回语句模板",
           "ret ${1:value}",
           "创建一个返回语句");
        sn("array", "数组类型模板",
           "[ ${1:type} * ${2:count} ]",
           "创建一个数组类型声明");
        sn("array-lit", "数组字面量模板",
           "[ ${1:element1}, ${2:element2} ]",
           "创建一个数组字面量");
        sn("loop", "循环模板",
           "loop {\n\t${1:body}\n\tif ${2:condition} {\n\t\tbreak;\n\t}\n}",
           "创建一个循环");
        sn("try", "try-catch 模板",
           "try {\n\t${1:body}\n} catch ${2:e} ${3:Err} {\n\t${4:handler}\n}",
           "创建一个 try-catch 错误处理块");
        sn("match", "match 模板",
           "match ${1:expr} {\n\t${2:Pattern} => ${3:result}\n\telse => ${4:default}\n}",
           "创建一个 match 模式匹配");
        sn("enum", "enum 声明模板",
           "enum ${1:Name} {\n\t${2:Variant1}\n\t${3:Variant2}(${4:T})\n}",
           "创建一个 enum 声明");

        return v;
    }();
    return items;
}

} // namespace

const std::vector<CompletionItem>& staticCompletions() {
    return buildItems();
}

} // namespace yux::lsp
