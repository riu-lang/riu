/*
 * Copyright (c) 2026. Yin-Jinlong@github
 */

import * as vscode from 'vscode';
import { YuxSymbolParser, YuxWorkspaceSymbols, YuxSymbol, StructInfo, FunctionInfo, VariableInfo } from './symbols';

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
    { label: 'loop', kind: vscode.CompletionItemKind.Keyword, detail: '循环语句' },
    { label: 'break', kind: vscode.CompletionItemKind.Keyword, detail: '跳出循环' },
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
    { label: 'String', kind: vscode.CompletionItemKind.Class, detail: '字符串类型' },
    { label: 'Ref', kind: vscode.CompletionItemKind.Class, detail: '引用类型 Ref<T>' },
    { label: 'Box', kind: vscode.CompletionItemKind.Class, detail: '堆对象类型 Box<T>' },
    { label: 'Ptr', kind: vscode.CompletionItemKind.Class, detail: '原始指针类型 Ptr<T>' },
    { label: 'Array', kind: vscode.CompletionItemKind.Class, detail: '动态数组类型 Array<T>' },
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
    {
        label: 'loop',
        kind: vscode.CompletionItemKind.Snippet,
        detail: '循环模板',
        insertText: 'loop {\n\t${1:body}\n\tif ${2:condition} {\n\t\tbreak;\n\t}\n}',
        documentation: '创建一个循环'
    },
];

const workspaceSymbols = new YuxWorkspaceSymbols();

export function activate(context: vscode.ExtensionContext) {
    console.log('yux-vscode extension is now active!');

    const helloCommand = vscode.commands.registerCommand('yux-vscode.helloWorld', () => {
        vscode.window.showInformationMessage('Hello World from yux-vscode!');
    });
    context.subscriptions.push(helloCommand);

    const symbolProvider = vscode.languages.registerDocumentSymbolProvider(
        { language: 'yux' },
        new YuxDocumentSymbolProvider()
    );
    context.subscriptions.push(symbolProvider);

    const definitionProvider = vscode.languages.registerDefinitionProvider(
        { language: 'yux' },
        new YuxDefinitionProvider()
    );
    context.subscriptions.push(definitionProvider);

    const completionProvider = vscode.languages.registerCompletionItemProvider(
        { language: 'yux' },
        new YuxCompletionProvider(),
        ' ', '.', '(', ')', '{', '}', '[', ']'
    );
    context.subscriptions.push(completionProvider);

    const referenceProvider = vscode.languages.registerReferenceProvider(
        { language: 'yux' },
        new YuxReferenceProvider()
    );
    context.subscriptions.push(referenceProvider);
}

class YuxDocumentSymbolProvider implements vscode.DocumentSymbolProvider {
    provideDocumentSymbols(document: vscode.TextDocument): vscode.ProviderResult<vscode.DocumentSymbol[]> {
        const parser = new YuxSymbolParser(document);
        const symbols = parser.parse();
        return symbols.map(s => {
            const symbol = new vscode.DocumentSymbol(
                s.name,
                s.detail || '',
                s.kind,
                s.range,
                s.selectionRange
            );
            return symbol;
        });
    }
}

class YuxDefinitionProvider implements vscode.DefinitionProvider {
    provideDefinition(document: vscode.TextDocument, position: vscode.Position): vscode.ProviderResult<vscode.Definition> {
        const wordRange = document.getWordRangeAtPosition(position);
        if (!wordRange) {
            return null;
        }

        const word = document.getText(wordRange);
        const parser = new YuxSymbolParser(document);

        const functions = parser.parseFunctions();
        for (const fn of functions) {
            if (fn.name === word) {
                return new vscode.Location(document.uri, fn.range);
            }
        }

        const variables = parser.parseVariables();
        for (const v of variables) {
            if (v.name === word) {
                return new vscode.Location(document.uri, v.range);
            }
        }

        const structs = parser.parseStructs();
        for (const [name, struct] of structs) {
            if (name === word) {
                return new vscode.Location(document.uri, struct.range);
            }

            for (const field of struct.fields) {
                if (field.name === word) {
                    return new vscode.Location(document.uri, field.range);
                }
            }

            for (const method of struct.methods) {
                if (method.name === word) {
                    return new vscode.Location(document.uri, method.range);
                }
            }
        }

        return null;
    }
}

class YuxCompletionProvider implements vscode.CompletionItemProvider {
    provideCompletionItems(document: vscode.TextDocument, position: vscode.Position): vscode.ProviderResult<vscode.CompletionItem[]> {
        const items: vscode.CompletionItem[] = [];
        const lineText = document.lineAt(position).text;
        const textBeforeCursor = lineText.substring(0, position.character);

        if (textBeforeCursor.endsWith('.')) {
            return this.provideMemberCompletions(document, position);
        }

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

        const parser = new YuxSymbolParser(document);
        const functions = parser.parseFunctions();
        for (const fn of functions) {
            const item = new vscode.CompletionItem(fn.name, vscode.CompletionItemKind.Function);
            item.detail = fn.returnType ? `(${fn.params}) ${fn.returnType}` : `(${fn.params})`;
            items.push(item);
        }

        const variables = parser.parseVariables();
        for (const v of variables) {
            const item = new vscode.CompletionItem(v.name, vscode.CompletionItemKind.Variable);
            item.detail = v.type || (v.isMutable ? 'var' : 'val');
            items.push(item);
        }

        const structs = parser.parseStructs();
        for (const [name] of structs) {
            const item = new vscode.CompletionItem(name, vscode.CompletionItemKind.Class);
            item.detail = 'struct';
            items.push(item);
        }

        return items;
    }

    private provideMemberCompletions(document: vscode.TextDocument, position: vscode.Position): vscode.CompletionItem[] {
        const items: vscode.CompletionItem[] = [];
        const lineText = document.lineAt(position).text;
        const textBeforeCursor = lineText.substring(0, position.character);

        const dotIndex = textBeforeCursor.lastIndexOf('.');
        if (dotIndex === -1) {
            return items;
        }

        const objectName = this.extractObjectName(textBeforeCursor.substring(0, dotIndex));
        if (!objectName) {
            return this.getDefaultMemberCompletions();
        }

        const objectType = this.findVariableType(document, objectName);
        if (!objectType) {
            return this.getDefaultMemberCompletions();
        }

        const parser = new YuxSymbolParser(document);
        const structs = parser.parseStructs();

        const struct = structs.get(objectType);
        if (struct) {
            for (const field of struct.fields) {
                const item = new vscode.CompletionItem(field.name, vscode.CompletionItemKind.Field);
                item.detail = field.type;
                items.push(item);
            }

            for (const method of struct.methods) {
                const item = new vscode.CompletionItem(method.name, vscode.CompletionItemKind.Method);
                item.detail = method.returnType ? `(${method.params}) ${method.returnType}` : `(${method.params})`;
                items.push(item);
            }
        }

        if (objectType === 'String') {
            items.push(...this.getStringMemberCompletions());
        }

        items.push(...this.getDefaultMemberCompletions());

        return items;
    }

    private extractObjectName(text: string): string | null {
        const match = text.match(/(\w+)\s*$/);
        return match ? match[1] : null;
    }

    private findVariableType(document: vscode.TextDocument, varName: string): string | null {
        const parser = new YuxSymbolParser(document);
        const variables = parser.parseVariables();

        for (const v of variables) {
            if (v.name === varName && v.type) {
                let type = v.type;
                if (type.startsWith('Ref<') || type.startsWith('Box<') || type.startsWith('Ptr<')) {
                    const innerMatch = type.match(/<(.+)>/);
                    if (innerMatch) {
                        type = innerMatch[1];
                    }
                }
                return type;
            }
        }

        return null;
    }

    private getDefaultMemberCompletions(): vscode.CompletionItem[] {
        const items: vscode.CompletionItem[] = [];

        for (const fn of TYPE_CONVERSION_FUNCTIONS) {
            const item = new vscode.CompletionItem(fn.label, vscode.CompletionItemKind.Method);
            item.detail = fn.detail;
            items.push(item);
        }

        const commonMethods = [
            { label: 'to_string', detail: '转换为字符串' },
        ];
        for (const m of commonMethods) {
            const item = new vscode.CompletionItem(m.label, vscode.CompletionItemKind.Method);
            item.detail = m.detail;
            items.push(item);
        }

        return items;
    }

    private getStringMemberCompletions(): vscode.CompletionItem[] {
        const items: vscode.CompletionItem[] = [];
        
        const stringMethods = [
            { label: 'len', detail: '返回字符串长度 () i64' },
            { label: 'clear', detail: '清空字符串 ()' },
            { label: 'append', detail: '追加内容 (cp u32) 或 (s String) 或 (b u8)' },
            { label: 'at', detail: '取码点 (i i64) u32' },
            { label: 'sub_string', detail: '截取子串 (start i64, end i64)' },
            { label: 'to_bytes', detail: '转为 UTF-8 字节数组 () Array<u8>' },
        ];
        
        for (const m of stringMethods) {
            const item = new vscode.CompletionItem(m.label, vscode.CompletionItemKind.Method);
            item.detail = m.detail;
            items.push(item);
        }
        
        return items;
    }
}

class YuxReferenceProvider implements vscode.ReferenceProvider {
    provideReferences(document: vscode.TextDocument, position: vscode.Position): vscode.ProviderResult<vscode.Location[]> {
        const wordRange = document.getWordRangeAtPosition(position);
        if (!wordRange) {
            return [];
        }

        const word = document.getText(wordRange);
        const locations: vscode.Location[] = [];
        const text = document.getText();
        const lines = text.split('\n');

        for (let i = 0; i < lines.length; i++) {
            const line = lines[i];
            let searchPos = 0;
            while (true) {
                const idx = line.indexOf(word, searchPos);
                if (idx === -1) {
                    break;
                }

                const isWordBoundary = (idx === 0 || !/\w/.test(line[idx - 1])) &&
                    (idx + word.length === line.length || !/\w/.test(line[idx + word.length]));

                if (isWordBoundary) {
                    locations.push(new vscode.Location(
                        document.uri,
                        new vscode.Range(i, idx, i, idx + word.length)
                    ));
                }
                searchPos = idx + 1;
            }
        }

        return locations;
    }
}

export function deactivate() {}
