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

                return items;
            }
        },
        ' ', '.', '(', ')', '{', '}'
    );

    context.subscriptions.push(completionProvider);
}

export function deactivate() {}
