/*
 * Copyright (c) 2026. Yin-Jinlong@github
 * MPL-2.0
 *
 * 轻量类型解析：
 * - 剥一层泛型壳：Ref<T> / Box<T> / Ptr<T> / Array<T> → T
 * - 解析点链 `a.b.c`：沿作用域找 a，按 struct 字段/方法逐步下钻
 * - `to_xxx()` → xxx
 */

import * as vscode from 'vscode';
import type { StructInfo } from '../symbols';
import { Scope, resolveVar, enclosingStruct } from './scope';

const WRAP_TYPES = ['Ref', 'Box', 'Ptr', 'Array'];

export function unwrapOne(type: string): string {
    const m = type.match(/^(\w+)<(.+)>$/);
    if (m && WRAP_TYPES.includes(m[1])) {
        return m[2].trim();
    }
    return type;
}

/**
 * 解析点链表达式（不含结尾的 `.`）的最终类型。
 * text 示例: "a", "a.b", "p.sum()", "x.to_i32()"
 */
export function resolveChainType(
    text: string,
    scope: Scope,
    pos: vscode.Position,
    structs: Map<string, StructInfo>,
): string | null {
    const parts = splitChain(text);
    if (parts.length === 0) {
        return null;
    }

    const head = parts[0];
    let curType: string | null;

    if (head.endsWith('()')) {
        const fnName = head.slice(0, -2);
        const conv = typeConversionReturn(fnName);
        curType = conv ?? null;
    } else if (head === 'self') {
        curType = enclosingStruct(scope) ?? null;
    } else {
        const v = resolveVar(scope, head, pos);
        curType = v ? v.type : null;
        if (v && !curType && v.initExpr) {
            curType = inferFromInit(v.initExpr, structs, scope, pos);
        }
    }

    if (!curType) {
        return null;
    }

    for (let i = 1; i < parts.length; i++) {
        curType = unwrapOne(curType);
        const struct = structs.get(curType);
        if (!struct) {
            return null;
        }
        const seg = parts[i];
        if (seg.endsWith('()')) {
            const mname = seg.slice(0, -2);
            const conv = typeConversionReturn(mname);
            if (conv) {
                curType = conv;
                continue;
            }
            const m = struct.methods.find((mm) => mm.name === mname);
            if (!m) {
                return null;
            }
            curType = m.returnType;
        } else {
            const f = struct.fields.find((ff) => ff.name === seg);
            if (!f) {
                return null;
            }
            curType = f.type;
        }
        if (!curType) {
            return null;
        }
    }

    return unwrapOne(curType);
}

/**
 * 从初始化表达式文本粗略推断类型：
 * - `StructName(...)` → StructName（已知 struct）
 * - `to_xxx(...)` → xxx
 * - 末尾是链式调用 `a.b.c()` 时递归走 resolveChainType
 */
function inferFromInit(
    expr: string,
    structs: Map<string, StructInfo>,
    scope: Scope,
    pos: vscode.Position,
): string | null {
    const text = expr.replace(/\s+/g, '');
    const ctor = text.match(/^([A-Za-z_]\w*)\(/);
    if (ctor && structs.has(ctor[1])) {
        return ctor[1];
    }
    const conv = text.match(/^to_([a-z0-9]+)\(/);
    if (conv) {
        const t = typeConversionReturn('to_' + conv[1]);
        if (t) {
            return t;
        }
    }
    if (/^[\w.()]+$/.test(text) && text.includes('.')) {
        return resolveChainType(text, scope, pos, structs);
    }
    return null;
}

function typeConversionReturn(name: string): string | null {
    if (!name.startsWith('to_')) {
        return null;
    }
    const t = name.slice(3);
    if (['i8', 'i16', 'i32', 'i64', 'u8', 'u16', 'u32', 'u64', 'f32', 'f64', 'bool'].includes(t)) {
        return t;
    }
    if (t === 'string') {
        return 'String';
    }
    return null;
}

/**
 * 把 "a.b.c()" 拆成 ["a", "b", "c()"]；忽略括号/尖括号内的点。
 */
function splitChain(text: string): string[] {
    const parts: string[] = [];
    let depth = 0;
    let buf = '';
    for (let i = 0; i < text.length; i++) {
        const c = text[i];
        if (c === '(' || c === '[' || c === '<') {
            depth++;
            buf += c;
        } else if (c === ')' || c === ']' || c === '>') {
            depth--;
            buf += c;
        } else if (c === '.' && depth === 0) {
            if (buf) {
                parts.push(buf);
            }
            buf = '';
        } else {
            buf += c;
        }
    }
    if (buf) {
        parts.push(buf);
    }
    return parts;
}
