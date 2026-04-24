/*
 * Copyright (c) 2026. Yin-Jinlong@github
 * MPL-2.0
 *
 * 对外符号接口。真正的抽取由 ast/visitor.ts 完成；
 * YuxSymbolParser 负责解析一次并缓存结果。
 */

import * as vscode from 'vscode';
import { parseYux } from './parser';
import { extractSymbols, ExtractedSymbols, ImportInfo } from './ast/visitor';
import { buildScopeTree, ScopeTree, findScopeAt, visibleVars, resolveVar, enclosingStruct, Scope } from './ast/scope';
import { resolveChainType } from './ast/typeResolver';
import { findProjectRoot, findSymbolInImports, resolvePackageSegment, collectImportedCompletions, collectImportedStructs, ImportedCompletion } from './ast/workspace';

export interface YuxSymbol {
    name: string;
    kind: vscode.SymbolKind;
    range: vscode.Range;
    selectionRange: vscode.Range;
    containerName?: string;
    detail?: string;
}

export interface StructInfo {
    name: string;
    range: vscode.Range;
    fields: { name: string; type: string; range: vscode.Range }[];
    methods: { name: string; params: string; returnType: string; range: vscode.Range }[];
}

export interface FunctionInfo {
    name: string;
    params: string;
    returnType: string;
    range: vscode.Range;
    isMethod: boolean;
    containerName?: string;
}

export interface VariableInfo {
    name: string;
    type: string;
    range: vscode.Range;
    isMutable: boolean;
}

export interface GlobalConstInfo {
    name: string;
    type: string;
    range: vscode.Range;
}

export class YuxSymbolParser {
    private document: vscode.TextDocument;
    private cachedExtract: ExtractedSymbols | null = null;
    private cachedScope: ScopeTree | null = null;

    constructor(document: vscode.TextDocument) {
        this.document = document;
    }

    private parseOnce(): { extract: ExtractedSymbols; scope: ScopeTree } {
        if (this.cachedExtract && this.cachedScope) {
            return { extract: this.cachedExtract, scope: this.cachedScope };
        }
        const text = this.document.getText();
        const { tree } = parseYux(text);
        this.cachedExtract = extractSymbols(tree);
        this.cachedScope = buildScopeTree(tree, text);
        return { extract: this.cachedExtract, scope: this.cachedScope };
    }

    parse(): YuxSymbol[] {
        return this.parseOnce().extract.symbols;
    }

    parseStructs(): Map<string, StructInfo> {
        return this.parseOnce().extract.structs;
    }

    /** 本地 + 已导入模块的 struct 合并表（本地优先）。 */
    allStructs(): Map<string, StructInfo> {
        const local = this.parseOnce().extract.structs;
        const root = findProjectRoot(this.document.uri.fsPath);
        if (!root) {
            return local;
        }
        const merged = new Map<string, StructInfo>(local);
        for (const [name, hit] of collectImportedStructs(root, this.parseImports())) {
            if (!merged.has(name)) {
                merged.set(name, hit.info);
            }
        }
        return merged;
    }

    /** 在导入模块中查找 struct，返回 (file, StructInfo)。 */
    findImportedStruct(name: string): { file: string; info: StructInfo } | null {
        const root = findProjectRoot(this.document.uri.fsPath);
        if (!root) {
            return null;
        }
        return collectImportedStructs(root, this.parseImports()).get(name) ?? null;
    }

    parseFunctions(): FunctionInfo[] {
        return this.parseOnce().extract.functions;
    }

    parseVariables(): VariableInfo[] {
        return this.parseOnce().extract.variables;
    }

    scopeAt(pos: vscode.Position): Scope {
        return findScopeAt(this.parseOnce().scope, pos);
    }

    visibleVariablesAt(pos: vscode.Position): VariableInfo[] {
        const scope = this.scopeAt(pos);
        return visibleVars(scope, pos).map((v) => ({
            name: v.name,
            type: v.type,
            range: v.nameRange,
            isMutable: v.isMutable,
        }));
    }

    resolveVariableAt(name: string, pos: vscode.Position): VariableInfo | null {
        const scope = this.scopeAt(pos);
        const v = resolveVar(scope, name, pos);
        return v ? { name: v.name, type: v.type, range: v.nameRange, isMutable: v.isMutable } : null;
    }

    resolveExprType(text: string, pos: vscode.Position): string | null {
        const scope = this.scopeAt(pos);
        return resolveChainType(text, scope, pos, this.allStructs());
    }

    enclosingStructAt(pos: vscode.Position): string | undefined {
        return enclosingStruct(this.scopeAt(pos));
    }

    parseImports(): ImportInfo[] {
        return this.parseOnce().extract.imports;
    }

    /** 光标在 use 语句的包段上时，返回目标文件/目录 Location；否则 null。 */
    resolveImportAt(pos: vscode.Position): vscode.Location | null {
        const root = findProjectRoot(this.document.uri.fsPath);
        if (!root) {
            return null;
        }
        for (const imp of this.parseImports()) {
            for (let i = 0; i < imp.pkgRanges.length; i++) {
                if (imp.pkgRanges[i].contains(pos)) {
                    return resolvePackageSegment(root, imp.pkgs.slice(0, i + 1));
                }
            }
        }
        return null;
    }

    /** 收集 `use` 引入的模块里所有顶层符号，用于补全。 */
    collectImportedCompletions(): ImportedCompletion {
        const root = findProjectRoot(this.document.uri.fsPath);
        if (!root) {
            return { functions: [], structs: [], constants: [] };
        }
        return collectImportedCompletions(root, this.parseImports());
    }

    /** 在当前文件 `use` 所引入的模块中查找顶层符号（非 method）。 */
    findImportedSymbol(name: string): vscode.Location | null {
        const root = findProjectRoot(this.document.uri.fsPath);
        if (!root) {
            return null;
        }
        const hit = findSymbolInImports(root, this.parseImports(), name);
        if (!hit) {
            return null;
        }
        return new vscode.Location(vscode.Uri.file(hit.file), hit.range);
    }
}

export class YuxWorkspaceSymbols {
    private cache: Map<string, { parser: YuxSymbolParser; version: number }> = new Map();

    getParser(document: vscode.TextDocument): YuxSymbolParser {
        const key = document.uri.toString();
        const cur = this.cache.get(key);
        if (cur && cur.version === document.version) {
            return cur.parser;
        }
        const parser = new YuxSymbolParser(document);
        this.cache.set(key, { parser, version: document.version });
        return parser;
    }

    getDocumentSymbols(document: vscode.TextDocument): YuxSymbol[] {
        return this.getParser(document).parse();
    }

    getStructs(document: vscode.TextDocument): Map<string, StructInfo> {
        return this.getParser(document).parseStructs();
    }

    getFunctions(document: vscode.TextDocument): FunctionInfo[] {
        return this.getParser(document).parseFunctions();
    }

    getVariables(document: vscode.TextDocument): VariableInfo[] {
        return this.getParser(document).parseVariables();
    }

    invalidate(document: vscode.TextDocument): void {
        this.cache.delete(document.uri.toString());
    }
}
