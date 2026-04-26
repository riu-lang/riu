/*
 * Copyright (c) 2026. Yin-Jinlong@github
 * MPL-2.0
 *
 * 作用域树：global → fn → 嵌套 block。每个 scope 记录覆盖范围 + 本地变量。
 * 按位置查询时，沿树向下找最深包含作用域，再向上收集"声明点在 pos 之前"的变量。
 */

import * as vscode from 'vscode';
import { ParserRuleContext, Token } from 'antlr4ng';
import {
    ProgramContext,
    FnContext,
    FnHeaderContext,
    StatementBlockContext,
    StatementDeclareAssignContext,
    StructImplContext,
    FnParamsContext,
} from '../gen/yuxParser';

export interface ScopeVar {
    name: string;
    type: string;
    isMutable: boolean;
    declPos: vscode.Position;
    nameRange: vscode.Range;
    initExpr?: string;
}

export type ScopeKind = 'global' | 'fn' | 'block';

export interface Scope {
    kind: ScopeKind;
    range: vscode.Range;
    parent: Scope | null;
    children: Scope[];
    vars: ScopeVar[];
    containerStruct?: string;
}

export interface ScopeTree {
    root: Scope;
}

export function buildScopeTree(tree: ProgramContext, source?: string): ScopeTree {
    const root: Scope = {
        kind: 'global',
        range: ctxRange(tree),
        parent: null,
        children: [],
        vars: [],
    };

    for (const fn of tree.fn()) {
        root.children.push(buildFnScope(fn, root));
    }
    for (const si of tree.structImpl()) {
        const implName = si._name?.text ?? '';
        for (const fn of si.fn()) {
            root.children.push(buildFnScope(fn, root, implName));
        }
    }

    const scopeTree: ScopeTree = { root };
    if (source) {
        applyRegexFallback(scopeTree, source);
    }
    return scopeTree;
}

/**
 * 当 ANTLR 错误恢复把大段 fn 体吞掉时，AST 里看不到 `val x = ...` 这样的声明。
 * 用行级正则扫一遍源码补齐缺失的变量，按位置归到最近的 scope。
 * 规则宽松，不关心嵌套/大括号平衡；重复的 name 不会覆盖 AST 已捕获的。
 */
function applyRegexFallback(tree: ScopeTree, source: string): void {
    const lines = source.split(/\r?\n/);
    const declRe = /^\s*(var|val|cval)\s+([A-Za-z_]\w*)\s*([A-Za-z_][\w<>,\s]*?)?\s*=\s*(.+?)\s*;?\s*$/;
    for (let i = 0; i < lines.length; i++) {
        const line = lines[i];
        if (/^\s*(\/\/|;|#)/.test(line)) {
            continue;
        }
        const m = line.match(declRe);
        if (!m) {
            continue;
        }
        const [, declKey, name, rawType, rawExpr] = m;
        const nameIdx = line.indexOf(name, line.indexOf(declKey) + declKey.length);
        if (nameIdx < 0) {
            continue;
        }
        const declPos = new vscode.Position(i, nameIdx);
        const scope = findScopeAt(tree, declPos);
        if (scope.vars.some((v) => v.name === name && v.declPos.line === i)) {
            continue;
        }
        const type = (rawType ?? '').trim();
        const initExpr = type ? undefined : rawExpr.trim();
        scope.vars.push({
            name,
            type,
            isMutable: declKey === 'var',
            declPos,
            nameRange: new vscode.Range(i, nameIdx, i, nameIdx + name.length),
            initExpr,
        });
    }
}

function buildFnScope(fn: FnContext, parent: Scope, containerStruct?: string): Scope {
    const header = fn.fnHeader();
    const scope: Scope = {
        kind: 'fn',
        range: ctxRange(fn),
        parent,
        children: [],
        vars: collectParams(header),
        containerStruct,
    };

    const blockScopes = new Map<StatementBlockContext, Scope>();
    collectNestedBlocks(fn, scope, blockScopes);
    collectDecls(fn, scope, blockScopes);
    return scope;
}

function collectParams(header: FnHeaderContext): ScopeVar[] {
    const out: ScopeVar[] = [];
    const paramsCtx = header.fnParams();
    if (!paramsCtx) {
        return out;
    }
    for (const paramCtx of paramsCtx.fnParam()) {
        const std = paramCtx.fnParamStd();
        if (std) {
            const nameTok = std._name;
            if (nameTok) {
                out.push({
                    name: nameTok.text ?? '',
                    type: std.type().getText(),
                    isMutable: false,
                    declPos: tokenStart(nameTok),
                    nameRange: tokenRange(nameTok),
                });
            }
            continue;
        }
        const group = paramCtx.fnParamGroup();
        if (group) {
            const type = group.type().getText();
            for (const nameTok of group._names) {
                out.push({
                    name: nameTok.text ?? '',
                    type,
                    isMutable: false,
                    declPos: tokenStart(nameTok),
                    nameRange: tokenRange(nameTok),
                });
            }
        }
    }
    return out;
}

/**
 * 递归找 fn ctx 里的所有 StatementBlockContext，建立 block scope。
 * 用 map 记录 blockCtx → scope，方便后续 declare-assign 按位置归属。
 */
function collectNestedBlocks(
    node: ParserRuleContext,
    parent: Scope,
    blockScopes: Map<StatementBlockContext, Scope>,
): void {
    const n = node.getChildCount();
    for (let i = 0; i < n; i++) {
        const c = node.getChild(i);
        if (c instanceof StatementBlockContext) {
            const sub: Scope = {
                kind: 'block',
                range: ctxRange(c),
                parent,
                children: [],
                vars: [],
            };
            parent.children.push(sub);
            blockScopes.set(c, sub);
            collectNestedBlocks(c, sub, blockScopes);
        } else if (c instanceof ParserRuleContext) {
            collectNestedBlocks(c, parent, blockScopes);
        }
    }
}

/** 深度遍历 fn，把所有 StatementDeclareAssignContext 归给最深的包含块（或 fn 本身）。 */
function collectDecls(
    fnCtx: ParserRuleContext,
    fnScope: Scope,
    blockScopes: Map<StatementBlockContext, Scope>,
): void {
    const visit = (node: ParserRuleContext) => {
        if (node instanceof StatementDeclareAssignContext) {
            const owner = enclosingBlockScope(node, fnScope, blockScopes);
            addDeclareAssign(node, owner);
            return;
        }
        const n = node.getChildCount();
        for (let i = 0; i < n; i++) {
            const c = node.getChild(i);
            if (c instanceof ParserRuleContext) {
                visit(c);
            }
        }
    };
    visit(fnCtx);
}

function enclosingBlockScope(
    ctx: ParserRuleContext,
    fnScope: Scope,
    blockScopes: Map<StatementBlockContext, Scope>,
): Scope {
    let cur: ParserRuleContext | null = ctx.parent as ParserRuleContext | null;
    while (cur) {
        if (cur instanceof StatementBlockContext) {
            const s = blockScopes.get(cur);
            if (s) {
                return s;
            }
        }
        cur = cur.parent as ParserRuleContext | null;
    }
    return fnScope;
}

function addDeclareAssign(ctx: StatementDeclareAssignContext, scope: Scope): void {
    const nameTok = ctx._name;
    if (!nameTok) {
        return;
    }
    const typeCtx = ctx.type();
    const type = typeCtx ? typeCtx.getText() : '';
    const declKey = ctx.DeclKey().getText();
    const exprCtx = ctx.expr();
    scope.vars.push({
        name: nameTok.text ?? '',
        type,
        isMutable: declKey === 'var',
        declPos: tokenStart(nameTok),
        nameRange: tokenRange(nameTok),
        initExpr: !type && exprCtx ? exprCtx.getText() : undefined,
    });
}

export function findScopeAt(tree: ScopeTree, pos: vscode.Position): Scope {
    let cur = tree.root;
    outer: while (true) {
        for (const ch of cur.children) {
            if (ch.range.contains(pos)) {
                cur = ch;
                continue outer;
            }
        }
        return cur;
    }
}

export function visibleVars(scope: Scope, pos: vscode.Position): ScopeVar[] {
    const seen = new Set<string>();
    const out: ScopeVar[] = [];
    let s: Scope | null = scope;
    while (s) {
        for (const v of s.vars) {
            if (v.declPos.isAfter(pos)) {
                continue;
            }
            if (!seen.has(v.name)) {
                seen.add(v.name);
                out.push(v);
            }
        }
        s = s.parent;
    }
    return out;
}

export function resolveVar(scope: Scope, name: string, pos: vscode.Position): ScopeVar | null {
    let s: Scope | null = scope;
    while (s) {
        for (let i = s.vars.length - 1; i >= 0; i--) {
            const v = s.vars[i];
            if (v.name !== name) {
                continue;
            }
            if (v.declPos.isAfter(pos)) {
                continue;
            }
            return v;
        }
        s = s.parent;
    }
    return null;
}

export function enclosingStruct(scope: Scope): string | undefined {
    let s: Scope | null = scope;
    while (s) {
        if (s.containerStruct) {
            return s.containerStruct;
        }
        s = s.parent;
    }
    return undefined;
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

function tokenStart(tok: Token): vscode.Position {
    return new vscode.Position((tok.line ?? 1) - 1, tok.column ?? 0);
}
