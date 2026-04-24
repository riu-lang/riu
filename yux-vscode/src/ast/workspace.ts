/*
 * Copyright (c) 2026. Yin-Jinlong@github
 * MPL-2.0
 *
 * 跨文件解析：
 * - 从 document 路径向上找 yux.toml，定位 projectRoot
 * - `use a.b` → <root>/a/b.yux 或 <root>/a/b/ 下所有 .yux
 * - `use a.b.*` → <root>/a/b/ 下所有 .yux（不递归子目录）
 * - 解析匹配文件，返回 fn/struct/globalConst 的 Location
 */

import * as vscode from 'vscode';
import * as fs from 'node:fs';
import * as path from 'node:path';
import { parseYux } from '../parser';
import { extractSymbols, ExtractedSymbols, ImportInfo } from './visitor';
import type { StructInfo } from '../symbols';

let sdkRootCache: string | null | undefined = undefined;

/** 从 PATH 里找 yux(.exe)，sdk = <yux.exe 所在目录>/../sdk；找不到返回 null。 */
export function findSdkRoot(): string | null {
    if (sdkRootCache !== undefined) {
        return sdkRootCache;
    }
    const envPath = process.env.PATH ?? process.env.Path ?? '';
    const sep = process.platform === 'win32' ? ';' : ':';
    const exeNames = process.platform === 'win32' ? ['yux.exe', 'yux'] : ['yux'];
    for (const dir of envPath.split(sep)) {
        if (!dir) {
            continue;
        }
        for (const name of exeNames) {
            const cand = path.join(dir, name);
            try {
                if (fs.existsSync(cand) && fs.statSync(cand).isFile()) {
                    const sdk = path.resolve(path.dirname(cand), '..', 'sdk');
                    if (fs.existsSync(sdk) && fs.statSync(sdk).isDirectory()) {
                        sdkRootCache = sdk;
                        return sdk;
                    }
                }
            } catch {
                // ignore
            }
        }
    }
    sdkRootCache = null;
    return null;
}

interface CachedFile {
    mtime: number;
    extract: ExtractedSymbols;
}

const fileCache = new Map<string, CachedFile>();

export function findProjectRoot(fsPath: string): string | null {
    let cur = path.dirname(fsPath);
    const root = path.parse(cur).root;
    while (cur && cur !== root) {
        if (fs.existsSync(path.join(cur, 'yux.toml'))) {
            return cur;
        }
        const parent = path.dirname(cur);
        if (parent === cur) {
            return null;
        }
        cur = parent;
    }
    return null;
}

/**
 * 把 `use a.b.*` / `use a.b` 展开成候选绝对路径列表。
 * 优先级：完全匹配文件 > 包目录下 .yux 文件。
 */
export function resolveImportFiles(projectRoot: string, imp: ImportInfo): string[] {
    const roots = [projectRoot];
    const sdk = findSdkRoot();
    if (sdk) {
        roots.push(sdk);
    }
    const out: string[] = [];
    for (const root of roots) {
        collectForRoot(root, imp, out);
        if (out.length > 0) {
            break;
        }
    }
    return out;
}

function collectForRoot(root: string, imp: ImportInfo, out: string[]): void {
    const rel = imp.pkgs.join('/');
    const fileCand = path.join(root, rel + '.yux');
    const dirCand = path.join(root, rel);
    if (imp.useAll) {
        if (fs.existsSync(dirCand) && fs.statSync(dirCand).isDirectory()) {
            for (const name of fs.readdirSync(dirCand)) {
                if (name.endsWith('.yux')) {
                    out.push(path.join(dirCand, name));
                }
            }
        }
        if (fs.existsSync(fileCand) && fs.statSync(fileCand).isFile()) {
            out.push(fileCand);
        }
    } else {
        if (fs.existsSync(fileCand) && fs.statSync(fileCand).isFile()) {
            out.push(fileCand);
        } else if (fs.existsSync(dirCand) && fs.statSync(dirCand).isDirectory()) {
            for (const name of fs.readdirSync(dirCand)) {
                if (name.endsWith('.yux')) {
                    out.push(path.join(dirCand, name));
                }
            }
        }
    }
}

function getFileSymbols(absPath: string): ExtractedSymbols | null {
    let mtime: number;
    try {
        mtime = fs.statSync(absPath).mtimeMs;
    } catch {
        return null;
    }
    const cached = fileCache.get(absPath);
    if (cached && cached.mtime === mtime) {
        return cached.extract;
    }
    let source: string;
    try {
        source = fs.readFileSync(absPath, 'utf8');
    } catch {
        return null;
    }
    const { tree } = parseYux(source);
    const extract = extractSymbols(tree);
    fileCache.set(absPath, { mtime, extract });
    return extract;
}

export interface ImportedSymbolHit {
    file: string;
    range: vscode.Range;
}

export function findSymbolInImports(
    projectRoot: string,
    imports: ImportInfo[],
    name: string,
): ImportedSymbolHit | null {
    for (const imp of imports) {
        const files = resolveImportFiles(projectRoot, imp);
        for (const f of files) {
            const ex = getFileSymbols(f);
            if (!ex) {
                continue;
            }
            const hit = findInExtract(ex, name);
            if (hit) {
                return { file: f, range: hit };
            }
        }
    }
    return null;
}

function findInExtract(ex: ExtractedSymbols, name: string): vscode.Range | null {
    for (const fn of ex.functions) {
        if (fn.name === name && !fn.isMethod) {
            return fn.range;
        }
    }
    const struct = ex.structs.get(name);
    if (struct) {
        return struct.range;
    }
    for (const s of ex.symbols) {
        if (s.kind === vscode.SymbolKind.Constant && s.name === name) {
            return s.selectionRange;
        }
    }
    return null;
}

/**
 * 光标落在 `use a.b.c` 的包段上时，返回对应文件/目录的 Location。
 * idx 是 pkgs 下标。
 */
export function resolvePackageSegment(projectRoot: string, pkgs: string[]): vscode.Location | null {
    const roots = [projectRoot];
    const sdk = findSdkRoot();
    if (sdk) {
        roots.push(sdk);
    }
    const rel = pkgs.join('/');
    for (const root of roots) {
        const fileCand = path.join(root, rel + '.yux');
        const dirCand = path.join(root, rel);
        if (fs.existsSync(fileCand) && fs.statSync(fileCand).isFile()) {
            return new vscode.Location(vscode.Uri.file(fileCand), new vscode.Position(0, 0));
        }
        if (fs.existsSync(dirCand) && fs.statSync(dirCand).isDirectory()) {
            return new vscode.Location(vscode.Uri.file(dirCand), new vscode.Position(0, 0));
        }
    }
    return null;
}

export interface ImportedStructHit {
    file: string;
    info: StructInfo;
}

/**
 * 收集所有 import 中的 struct 定义 (name -> {file, info})，优先第一次出现的定义。
 */
export function collectImportedStructs(
    projectRoot: string,
    imports: ImportInfo[],
): Map<string, ImportedStructHit> {
    const out = new Map<string, ImportedStructHit>();
    for (const imp of imports) {
        for (const f of resolveImportFiles(projectRoot, imp)) {
            const ex = getFileSymbols(f);
            if (!ex) {
                continue;
            }
            for (const [name, info] of ex.structs) {
                if (!out.has(name)) {
                    out.set(name, { file: f, info });
                }
            }
        }
    }
    return out;
}

export interface ImportedCompletion {
    functions: { name: string; params: string; returnType: string }[];
    structs: string[];
    constants: { name: string; detail?: string }[];
}

export function collectImportedCompletions(projectRoot: string, imports: ImportInfo[]): ImportedCompletion {
    const out: ImportedCompletion = { functions: [], structs: [], constants: [] };
    const seenFn = new Set<string>();
    const seenStruct = new Set<string>();
    const seenConst = new Set<string>();
    for (const imp of imports) {
        for (const f of resolveImportFiles(projectRoot, imp)) {
            const ex = getFileSymbols(f);
            if (!ex) {
                continue;
            }
            for (const fn of ex.functions) {
                if (fn.isMethod || seenFn.has(fn.name)) {
                    continue;
                }
                seenFn.add(fn.name);
                out.functions.push({ name: fn.name, params: fn.params, returnType: fn.returnType });
            }
            for (const name of ex.structs.keys()) {
                if (seenStruct.has(name)) {
                    continue;
                }
                seenStruct.add(name);
                out.structs.push(name);
            }
            for (const s of ex.symbols) {
                if (s.kind !== vscode.SymbolKind.Constant || seenConst.has(s.name)) {
                    continue;
                }
                seenConst.add(s.name);
                out.constants.push({ name: s.name, detail: s.detail });
            }
        }
    }
    return out;
}

export function invalidateFileCache(absPath?: string): void {
    if (absPath) {
        fileCache.delete(absPath);
    } else {
        fileCache.clear();
    }
}
