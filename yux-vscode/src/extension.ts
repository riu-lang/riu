/*
 * Copyright (c) 2026. Yin-Jinlong@github
 */

import * as vscode from 'vscode';

const KEYWORDS = [
    { label: 'fn', kind: vscode.CompletionItemKind.Keyword, detail: '函数声明' },
    { label: 'ret', kind: vscode.CompletionItemKind.Keyword, detail: '返回语句' },
    { label: 'if', kind: vscode.CompletionItemKind.Keyword, detail: '条件语句' },
    { label: 'elif', kind: vscode.CompletionItemKind.Keyword, detail: 'else if 条件' },
    { label: 'else', kind: vscode.CompletionItemKind.Keyword, detail: 'else 分支' },
    { label: 'true', kind: vscode.CompletionItemKind.Keyword, detail: '布尔真值' },
    { label: 'false', kind: vscode.CompletionItemKind.Keyword, detail: '布尔假值' },
    { label: 'null', kind: vscode.CompletionItemKind.Keyword, detail: '空值' },
    { label: 'var', kind: vscode.CompletionItemKind.Keyword, detail: '可变变量声明' },
    { label: 'val', kind: vscode.CompletionItemKind.Keyword, detail: '不可变变量声明' },
    { label: 'cval', kind: vscode.CompletionItemKind.Keyword, detail: '编译时常量声明' },
    { label: 'struct', kind: vscode.CompletionItemKind.Keyword, detail: '结构体声明' },
];

const TYPES = [
    { label: 'i8', kind: vscode.CompletionItemKind.Class, detail: '8位有符号整数' },
    { label: 'i16', kind: vscode.CompletionItemKind.Class, detail: '16位有符号整数' },
    { label: 'i32', kind: vscode.CompletionItemKind.Class, detail: '32位有符号整数' },
    { label: 'i64', kind: vscode.CompletionItemKind.Class, detail: '64位有符号整数' },
    { label: 'u8', kind: vscode.CompletionItemKind.Class, detail: '8位无符号整数' },
    { label: 'u16', kind: vscode.CompletionItemKind.Class, detail: '16位无符号整数' },
    { label: 'u32', kind: vscode.CompletionItemKind.Class, detail: '32位无符号整数' },
    { label: 'u64', kind: vscode.CompletionItemKind.Class, detail: '64位无符号整数' },
    { label: 'f32', kind: vscode.CompletionItemKind.Class, detail: '32位浮点数' },
    { label: 'f64', kind: vscode.CompletionItemKind.Class, detail: '64位浮点数' },
    { label: 'bool', kind: vscode.CompletionItemKind.Class, detail: '布尔类型' },
];

const BUILTIN_FUNCTIONS = [
    { label: 'print', kind: vscode.CompletionItemKind.Function, detail: '内置函数：打印输出' },
    { label: 'println', kind: vscode.CompletionItemKind.Function, detail: '内置函数：打印输出并换行' },
];

const TYPE_NAMES = ['bool', 'i8', 'i16', 'i32', 'i64', 'u8', 'u16', 'u32', 'u64', 'f32', 'f64'];

const TYPE_CONVERSION_FUNCTIONS = TYPE_NAMES.map(t => ({
    label: `to_${t}`,
    kind: vscode.CompletionItemKind.Function,
    detail: `类型转换：转换为 ${t}`
}));

const SNIPPETS = [
    {
        label: 'fn',
        kind: vscode.CompletionItemKind.Snippet,
        detail: '函数声明模板',
        insertText: 'fn ${1:name}(${2:params}) ${3:retType} {\n\t${4:body}\n}',
        documentation: '创建一个新函数'
    },
    {
        label: 'fn-expr',
        kind: vscode.CompletionItemKind.Snippet,
        detail: '函数表达式模板',
        insertText: 'fn ${1:name}(${2:params}) = ${3:expr}',
        documentation: '创建一个表达式函数'
    },
    {
        label: 'if',
        kind: vscode.CompletionItemKind.Snippet,
        detail: 'if 条件模板',
        insertText: 'if ${1:condition} {\n\t${2:body}\n}',
        documentation: '创建一个 if 条件语句'
    },
    {
        label: 'if-else',
        kind: vscode.CompletionItemKind.Snippet,
        detail: 'if-else 条件模板',
        insertText: 'if ${1:condition} {\n\t${2:body}\n} else {\n\t${3:elseBody}\n}',
        documentation: '创建一个 if-else 条件语句'
    },
    {
        label: 'if-elif-else',
        kind: vscode.CompletionItemKind.Snippet,
        detail: 'if-elif-else 条件模板',
        insertText: 'if ${1:condition} {\n\t${2:body}\n} elif ${3:condition2} {\n\t${4:body2}\n} else {\n\t${5:elseBody}\n}',
        documentation: '创建一个完整的 if-elif-else 条件语句'
    },
    {
        label: 'struct',
        kind: vscode.CompletionItemKind.Snippet,
        detail: '结构体声明模板',
        insertText: 'struct ${1:Name} {\n\t${2:field} ${3:type}\n}',
        documentation: '创建一个结构体声明'
    },
    {
        label: 'impl',
        kind: vscode.CompletionItemKind.Snippet,
        detail: '结构体实现模板',
        insertText: '${1:Name} {\n\tfn ${2:method}(${3:params}) {\n\t\t${4:body}\n\t}\n}',
        documentation: '创建一个结构体实现块'
    },
    {
        label: 'var',
        kind: vscode.CompletionItemKind.Snippet,
        detail: '可变变量声明模板',
        insertText: 'var ${1:name} ${2:type} = ${3:value}',
        documentation: '创建一个可变变量声明'
    },
    {
        label: 'val',
        kind: vscode.CompletionItemKind.Snippet,
        detail: '不可变变量声明模板',
        insertText: 'val ${1:name} ${2:type} = ${3:value}',
        documentation: '创建一个不可变变量声明'
    },
    {
        label: 'cval',
        kind: vscode.CompletionItemKind.Snippet,
        detail: '编译时常量声明模板',
        insertText: 'cval ${1:name} ${2:type} = ${3:value}',
        documentation: '创建一个编译时常量声明'
    },
    {
        label: 'ret',
        kind: vscode.CompletionItemKind.Snippet,
        detail: '返回语句模板',
        insertText: 'ret ${1:value}',
        documentation: '创建一个返回语句'
    },
    {
        label: 'array',
        kind: vscode.CompletionItemKind.Snippet,
        detail: '数组类型模板',
        insertText: '[ ${1:type} * ${2:count} ]',
        documentation: '创建一个数组类型声明'
    },
    {
        label: 'array-lit',
        kind: vscode.CompletionItemKind.Snippet,
        detail: '数组字面量模板',
        insertText: '[ ${1:element1}, ${2:element2} ]',
        documentation: '创建一个数组字面量'
    },
];

export function activate(context: vscode.ExtensionContext) {
    console.log('Congratulations, your extension "yux-vscode" is now active!');

    const disposable = vscode.commands.registerCommand('yux-vscode.helloWorld', () => {
        vscode.window.showInformationMessage('Hello World from yux-vscode!');
    });

    context.subscriptions.push(disposable);

    const completionProvider = vscode.languages.registerCompletionItemProvider(
        { language: 'yux' },
        {
            provideCompletionItems(document: vscode.TextDocument, position: vscode.Position) {
                const items: vscode.CompletionItem[] = [];

                for (const kw of KEYWORDS) {
                    const item = new vscode.CompletionItem(kw.label, kw.kind);
                    item.detail = kw.detail;
                    items.push(item);
                }

                for (const t of TYPES) {
                    const item = new vscode.CompletionItem(t.label, t.kind);
                    item.detail = t.detail;
                    items.push(item);
                }

                for (const fn of BUILTIN_FUNCTIONS) {
                    const item = new vscode.CompletionItem(fn.label, fn.kind);
                    item.detail = fn.detail;
                    items.push(item);
                }

                for (const fn of TYPE_CONVERSION_FUNCTIONS) {
                    const item = new vscode.CompletionItem(fn.label, fn.kind);
                    item.detail = fn.detail;
                    items.push(item);
                }

                for (const snip of SNIPPETS) {
                    const item = new vscode.CompletionItem(snip.label, snip.kind);
                    item.detail = snip.detail;
                    item.insertText = new vscode.SnippetString(snip.insertText);
                    item.documentation = new vscode.MarkdownString(snip.documentation);
                    items.push(item);
                }

                return items;
            }
        },
        ' ', '.', '(', ')', '{', '}', '[', ']'
    );

    context.subscriptions.push(completionProvider);
}

export function deactivate() {}
