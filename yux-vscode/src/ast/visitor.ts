/*
 * Copyright (c) 2026. Yin-Jinlong@github
 * MPL-2.0
 *
 * 从 antlr4ng 的 parse tree 抽取符号信息，填充和 symbols.ts 同构的结构。
 * 只做一次遍历，缓存结果在 YuxSymbolParser 里。
 */

import * as vscode from 'vscode';
import { ParserRuleContext, Token } from 'antlr4ng';
import {
    ProgramContext,
    FnContext,
    FnHeaderContext,
    StructDeclContext,
    StructImplContext,
    GlobalConstContext,
    StatementDeclareAssignContext,
    TypeContext,
    ImportsContext,
} from '../gen/yuxParser';
import type { YuxSymbol, StructInfo, FunctionInfo, VariableInfo } from '../symbols';

export interface ImportInfo {
    pkgs: string[];
    useAll: boolean;
    range: vscode.Range;
    pkgRanges: vscode.Range[];
}

export interface ExtractedSymbols {
    symbols: YuxSymbol[];
    structs: Map<string, StructInfo>;
    functions: FunctionInfo[];
    variables: VariableInfo[];
    imports: ImportInfo[];
}

export function extractSymbols(tree: ProgramContext): ExtractedSymbols {
    const out: ExtractedSymbols = {
        symbols: [],
        structs: new Map(),
        functions: [],
        variables: [],
        imports: [],
    };

    for (const imp of tree.imports()) {
        visitImport(imp, out);
    }

    for (const s of tree.structDecl()) {
        visitStructDecl(s, out);
    }
    for (const s of tree.structImpl()) {
        visitStructImpl(s, out);
    }
    for (const fn of tree.fn()) {
        visitTopFn(fn, out);
    }
    for (const gc of tree.globalConst()) {
        visitGlobalConst(gc, out);
    }

    walk(tree, (ctx) => {
        if (ctx instanceof StatementDeclareAssignContext) {
            visitDeclareAssign(ctx, out);
        }
    });

    return out;
}

function visitImport(ctx: ImportsContext, out: ExtractedSymbols): void {
    const pkgs = ctx._pkgs.map((t) => t.text ?? '');
    if (pkgs.length === 0) {
        return;
    }
    out.imports.push({
        pkgs,
        useAll: !!ctx._useAll,
        range: ctxRange(ctx),
        pkgRanges: ctx._pkgs.map((t) => tokenRange(t)),
    });
}

function visitStructDecl(ctx: StructDeclContext, out: ExtractedSymbols): void {
    const nameTok = ctx._name;
    if (!nameTok) {
        return;
    }
    const name = nameTok.text ?? '';
    const range = ctxRange(ctx);
    const selRange = tokenRange(nameTok);

    out.symbols.push({
        name,
        kind: vscode.SymbolKind.Struct,
        range,
        selectionRange: selRange,
        detail: 'struct',
    });

    const fields: StructInfo['fields'] = [];
    for (const f of ctx.filedDecl()) {
        const fname = f._name;
        if (!fname) {
            continue;
        }
        fields.push({
            name: fname.text ?? '',
            type: f.type().getText(),
            range: tokenRange(fname),
        });
    }

    const existing = out.structs.get(name);
    if (existing) {
        existing.fields.push(...fields);
    } else {
        out.structs.set(name, { name, range, fields, methods: [] });
    }
}

function visitStructImpl(ctx: StructImplContext, out: ExtractedSymbols): void {
    const nameTok = ctx._name;
    if (!nameTok) {
        return;
    }
    const implName = nameTok.text ?? '';

    let struct = out.structs.get(implName);
    if (!struct) {
        struct = {
            name: implName,
            range: ctxRange(ctx),
            fields: [],
            methods: [],
        };
        out.structs.set(implName, struct);
    }

    for (const fn of ctx.fn()) {
        const header = fn.fnHeader();
        const info = fnHeaderInfo(header, implName);
        if (!info) {
            continue;
        }
        out.functions.push({ ...info, isMethod: true, containerName: implName });
        struct.methods.push({
            name: info.name,
            params: info.params,
            returnType: info.returnType,
            range: info.range,
        });

        const nameTokF = header._name;
        if (nameTokF) {
            let kind = vscode.SymbolKind.Method;
            if (info.name === implName) {
                kind = vscode.SymbolKind.Constructor;
            }
            out.symbols.push({
                name: info.name,
                kind,
                range: ctxRange(fn),
                selectionRange: tokenRange(nameTokF),
                containerName: implName,
                detail: info.returnType ? `(${info.params}) ${info.returnType}` : `(${info.params})`,
            });
        }
    }
}

function visitTopFn(fn: FnContext, out: ExtractedSymbols): void {
    const header = fn.fnHeader();
    const info = fnHeaderInfo(header);
    if (!info) {
        return;
    }
    out.functions.push({ ...info, isMethod: false });

    const nameTok = header._name;
    if (nameTok) {
        const kind = info.name.startsWith('~') ? vscode.SymbolKind.Constructor : vscode.SymbolKind.Function;
        out.symbols.push({
            name: info.name,
            kind,
            range: ctxRange(fn),
            selectionRange: tokenRange(nameTok),
            detail: info.returnType ? `(${info.params}) ${info.returnType}` : `(${info.params})`,
        });
    }
}

function visitGlobalConst(ctx: GlobalConstContext, out: ExtractedSymbols): void {
    const nameTok = ctx._name;
    if (!nameTok) {
        return;
    }
    const name = nameTok.text ?? '';
    const type = ctx.type().getText();

    out.symbols.push({
        name,
        kind: vscode.SymbolKind.Constant,
        range: ctxRange(ctx),
        selectionRange: tokenRange(nameTok),
        detail: type || 'const',
    });
}

function visitDeclareAssign(ctx: StatementDeclareAssignContext, out: ExtractedSymbols): void {
    const nameTok = ctx._name;
    if (!nameTok) {
        return;
    }
    const name = nameTok.text ?? '';
    const typeCtx = ctx.type();
    const type = typeCtx ? typeCtx.getText() : '';
    const declKey = ctx.DeclKey().getText();

    out.variables.push({
        name,
        type,
        range: tokenRange(nameTok),
        isMutable: declKey === 'var',
    });

    out.symbols.push({
        name,
        kind: declKey === 'cval' ? vscode.SymbolKind.Constant : vscode.SymbolKind.Variable,
        range: ctxRange(ctx),
        selectionRange: tokenRange(nameTok),
        detail: type || declKey,
    });
}

interface FnHeaderBasic {
    name: string;
    params: string;
    returnType: string;
    range: vscode.Range;
}

function fnHeaderInfo(header: FnHeaderContext, _container?: string): FnHeaderBasic | null {
    const nameTok = header._name;
    if (!nameTok) {
        return null;
    }
    const params = header._params.map((p) => p.getText()).join(', ');
    const retTypeCtx: TypeContext | null = header._retType ?? null;
    const returnType = retTypeCtx ? retTypeCtx.getText() : '';

    return {
        name: nameTok.text ?? '',
        params,
        returnType,
        range: ctxRange(header),
    };
}

function ctxRange(ctx: ParserRuleContext): vscode.Range {
    const start = ctx.start;
    const stop = ctx.stop ?? start;
    if (!start) {
        return new vscode.Range(0, 0, 0, 0);
    }
    return new vscode.Range(
        (start.line ?? 1) - 1,
        start.column ?? 0,
        (stop?.line ?? start.line ?? 1) - 1,
        (stop?.column ?? start.column ?? 0) + (stop?.text?.length ?? 0),
    );
}

function tokenRange(tok: Token): vscode.Range {
    const line = (tok.line ?? 1) - 1;
    const col = tok.column ?? 0;
    const len = tok.text?.length ?? 0;
    return new vscode.Range(line, col, line, col + len);
}

function walk(ctx: ParserRuleContext, cb: (c: ParserRuleContext) => void): void {
    cb(ctx);
    const n = ctx.getChildCount();
    for (let i = 0; i < n; i++) {
        const child = ctx.getChild(i);
        if (child instanceof ParserRuleContext) {
            walk(child, cb);
        }
    }
}
