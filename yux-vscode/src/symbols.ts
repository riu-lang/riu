/*
 * Copyright (c) 2026. Yin-Jinlong@github
 */

import * as vscode from 'vscode';

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

    constructor(document: vscode.TextDocument) {
        this.document = document;
    }

    parse(): YuxSymbol[] {
        const symbols: YuxSymbol[] = [];
        const text = this.document.getText();
        const lines = text.split('\n');

        let currentStructImpl: string | undefined;

        for (let i = 0; i < lines.length; i++) {
            const line = lines[i];
            const lineNum = i;

            const structImplMatch = this.matchStructImplStart(line);
            if (structImplMatch) {
                currentStructImpl = structImplMatch.name;
                continue;
            }

            if (currentStructImpl && line.trim() === '}') {
                currentStructImpl = undefined;
                continue;
            }

            const structSymbol = this.parseStructDefinition(line, lineNum);
            if (structSymbol) {
                symbols.push(structSymbol);
                continue;
            }

            const fnSymbol = this.parseFunctionDefinition(line, lineNum, currentStructImpl);
            if (fnSymbol) {
                symbols.push(fnSymbol);
                continue;
            }

            const varSymbol = this.parseVariableDeclaration(line, lineNum);
            if (varSymbol) {
                symbols.push(varSymbol);
                continue;
            }

            const constSymbol = this.parseGlobalConst(line, lineNum);
            if (constSymbol) {
                symbols.push(constSymbol);
            }
        }

        return symbols;
    }

    parseStructs(): Map<string, StructInfo> {
        const structs = new Map<string, StructInfo>();
        const text = this.document.getText();
        const lines = text.split('\n');

        let currentStruct: StructInfo | null = null;
        let currentImpl: string | null = null;
        let braceDepth = 0;

        for (let i = 0; i < lines.length; i++) {
            const line = lines[i];
            const trimmed = line.trim();

            if (currentStruct) {
                if (trimmed === '}') {
                    structs.set(currentStruct.name, currentStruct);
                    currentStruct = null;
                    continue;
                }

                const fieldMatch = trimmed.match(/^(\w+)\s+(\S+)/);
                if (fieldMatch && !trimmed.startsWith('fn ') && !trimmed.startsWith('/')) {
                    currentStruct.fields.push({
                        name: fieldMatch[1],
                        type: fieldMatch[2],
                        range: new vscode.Range(i, line.indexOf(fieldMatch[1]), i, line.indexOf(fieldMatch[1]) + fieldMatch[1].length)
                    });
                }
            } else if (currentImpl) {
                if (trimmed === '}') {
                    currentImpl = null;
                    continue;
                }

                const methodMatch = this.parseMethodDefinition(trimmed, i, currentImpl);
                if (methodMatch && structs.has(currentImpl)) {
                    const struct = structs.get(currentImpl)!;
                    struct.methods.push(methodMatch);
                }
            } else {
                const structMatch = trimmed.match(/^struct\s+(\w+)\s*\{/);
                if (structMatch) {
                    currentStruct = {
                        name: structMatch[1],
                        range: new vscode.Range(i, 0, i, line.length),
                        fields: [],
                        methods: []
                    };
                }

                const implMatch = trimmed.match(/^(\w+)\s*\{$/);
                if (implMatch) {
                    currentImpl = implMatch[1];
                }
            }
        }

        return structs;
    }

    parseFunctions(): FunctionInfo[] {
        const functions: FunctionInfo[] = [];
        const text = this.document.getText();
        const lines = text.split('\n');

        let currentImpl: string | null = null;

        for (let i = 0; i < lines.length; i++) {
            const line = lines[i];
            const trimmed = line.trim();

            const implMatch = trimmed.match(/^(\w+)\s*\{$/);
            if (implMatch) {
                currentImpl = implMatch[1];
                continue;
            }

            if (currentImpl && trimmed === '}') {
                currentImpl = null;
                continue;
            }

            const fnMatch = trimmed.match(/^fn\s+(\w+)\s*\(([^)]*)\)(?:\s*(\S+))?\s*[\{=]/);
            if (fnMatch) {
                functions.push({
                    name: fnMatch[1],
                    params: fnMatch[2].trim(),
                    returnType: fnMatch[3] || '',
                    range: new vscode.Range(i, line.indexOf('fn'), i, line.length),
                    isMethod: currentImpl !== null,
                    containerName: currentImpl || undefined
                });
            }
        }

        return functions;
    }

    parseVariables(): VariableInfo[] {
        const variables: VariableInfo[] = [];
        const text = this.document.getText();
        const lines = text.split('\n');

        for (let i = 0; i < lines.length; i++) {
            const line = lines[i];
            const trimmed = line.trim();

            const varMatch = trimmed.match(/^(var|val)\s+(\w+)(?:\s+(\S+))?\s*=/);
            if (varMatch) {
                variables.push({
                    name: varMatch[2],
                    type: varMatch[3] || '',
                    range: new vscode.Range(i, line.indexOf(varMatch[2]), i, line.indexOf(varMatch[2]) + varMatch[2].length),
                    isMutable: varMatch[1] === 'var'
                });
            }
        }

        return variables;
    }

    private matchStructImplStart(line: string): { name: string } | null {
        const trimmed = line.trim();
        const match = trimmed.match(/^(\w+)\s*\{$/);
        if (match && !trimmed.startsWith('struct ') && !trimmed.startsWith('fn ')) {
            return { name: match[1] };
        }
        return null;
    }

    private parseStructDefinition(line: string, lineNum: number): YuxSymbol | null {
        const trimmed = line.trim();
        const match = trimmed.match(/^struct\s+(\w+)\s*\{/);
        if (match) {
            const name = match[1];
            const startPos = line.indexOf(name);
            return {
                name,
                kind: vscode.SymbolKind.Struct,
                range: new vscode.Range(lineNum, 0, lineNum, line.length),
                selectionRange: new vscode.Range(lineNum, startPos, lineNum, startPos + name.length),
                detail: 'struct'
            };
        }
        return null;
    }

    private parseFunctionDefinition(line: string, lineNum: number, containerName?: string): YuxSymbol | null {
        const trimmed = line.trim();
        const match = trimmed.match(/^fn\s+(\w+)\s*\(([^)]*)\)(?:\s*(\S+))?\s*[\{=]/);
        if (match) {
            const name = match[1];
            const params = match[2].trim();
            const returnType = match[3] || '';
            const startPos = line.indexOf(name);

            let kind = vscode.SymbolKind.Function;
            if (name.startsWith('~')) {
                kind = vscode.SymbolKind.Constructor;
            } else if (containerName && name === containerName) {
                kind = vscode.SymbolKind.Constructor;
            }

            return {
                name,
                kind,
                range: new vscode.Range(lineNum, 0, lineNum, line.length),
                selectionRange: new vscode.Range(lineNum, startPos, lineNum, startPos + name.length),
                containerName,
                detail: returnType ? `(${params}) ${returnType}` : `(${params})`
            };
        }
        return null;
    }

    private parseVariableDeclaration(line: string, lineNum: number): YuxSymbol | null {
        const trimmed = line.trim();
        const match = trimmed.match(/^(var|val)\s+(\w+)(?:\s+(\S+))?\s*=/);
        if (match) {
            const name = match[2];
            const type = match[3] || '';
            const startPos = line.indexOf(name);
            return {
                name,
                kind: vscode.SymbolKind.Variable,
                range: new vscode.Range(lineNum, 0, lineNum, line.length),
                selectionRange: new vscode.Range(lineNum, startPos, lineNum, startPos + name.length),
                detail: type || (match[1] === 'var' ? 'var' : 'val')
            };
        }
        return null;
    }

    private parseGlobalConst(line: string, lineNum: number): YuxSymbol | null {
        const trimmed = line.trim();
        const match = trimmed.match(/^cval\s+(\w+)(?:\s+(\S+))?\s*=/);
        if (match) {
            const name = match[1];
            const type = match[2] || '';
            const startPos = line.indexOf(name);
            return {
                name,
                kind: vscode.SymbolKind.Constant,
                range: new vscode.Range(lineNum, 0, lineNum, line.length),
                selectionRange: new vscode.Range(lineNum, startPos, lineNum, startPos + name.length),
                detail: type || 'const'
            };
        }
        return null;
    }

    private parseMethodDefinition(line: string, lineNum: number, structName: string): { name: string; params: string; returnType: string; range: vscode.Range } | null {
        const match = line.match(/^fn\s+(\w+)\s*\(([^)]*)\)(?:\s*(\S+))?\s*[\{=]/);
        if (match) {
            return {
                name: match[1],
                params: match[2].trim(),
                returnType: match[3] || '',
                range: new vscode.Range(lineNum, 0, lineNum, line.length)
            };
        }
        return null;
    }
}

export class YuxWorkspaceSymbols {
    private cache: Map<string, { symbols: YuxSymbol[]; structs: Map<string, StructInfo>; functions: FunctionInfo[]; variables: VariableInfo[] }> = new Map();

    getDocumentSymbols(document: vscode.TextDocument): YuxSymbol[] {
        const cached = this.cache.get(document.uri.toString());
        if (cached) {
            return cached.symbols;
        }
        const parser = new YuxSymbolParser(document);
        const symbols = parser.parse();
        return symbols;
    }

    getStructs(document: vscode.TextDocument): Map<string, StructInfo> {
        const parser = new YuxSymbolParser(document);
        return parser.parseStructs();
    }

    getFunctions(document: vscode.TextDocument): FunctionInfo[] {
        const parser = new YuxSymbolParser(document);
        return parser.parseFunctions();
    }

    getVariables(document: vscode.TextDocument): VariableInfo[] {
        const parser = new YuxSymbolParser(document);
        return parser.parseVariables();
    }

    invalidate(document: vscode.TextDocument): void {
        this.cache.delete(document.uri.toString());
    }
}
