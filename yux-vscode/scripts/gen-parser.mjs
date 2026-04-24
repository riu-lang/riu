/*
 * Copyright (c) 2026. Yin-Jinlong@github
 * MPL-2.0
 *
 * 从 ../src/yux.g4 生成 TypeScript parser 到 src/gen/。
 * .g4 的 options { language=Cpp; } 由 -D language=TypeScript 覆盖，文件不改。
 */

import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { dirname, resolve, join } from 'node:path';
import { mkdirSync, rmSync, existsSync, readFileSync, writeFileSync } from 'node:fs';

const __filename = fileURLToPath(import.meta.url);
const __dirname = dirname(__filename);

const extRoot = resolve(__dirname, '..');
const repoRoot = resolve(extRoot, '..');
const grammar = resolve(repoRoot, 'src', 'yux.g4');
const outDir = resolve(extRoot, 'src', 'gen');

if (!existsSync(grammar)) {
    console.error(`[gen-parser] grammar not found: ${grammar}`);
    process.exit(1);
}

const antlrPkgDir = join(extRoot, 'node_modules', 'antlr-ng');
const antlrPkgJsonPath = join(antlrPkgDir, 'package.json');
if (!existsSync(antlrPkgJsonPath)) {
    console.error(`[gen-parser] antlr-ng not installed at ${antlrPkgDir}; run 'pnpm install'`);
    process.exit(1);
}
const antlrPkg = JSON.parse(readFileSync(antlrPkgJsonPath, 'utf8'));
const antlrBin = resolve(antlrPkgDir, antlrPkg.bin['antlr-ng']);

if (existsSync(outDir)) {
    rmSync(outDir, { recursive: true, force: true });
}
mkdirSync(outDir, { recursive: true });

const args = [
    antlrBin,
    '-D', 'language=TypeScript',
    '-v', 'true',
    '-l', 'true',
    '-o', outDir,
    '--exact-output-dir', 'true',
    grammar,
];

console.log(`[gen-parser] node ${args.join(' ')}`);

const result = spawnSync(process.execPath, args, {
    cwd: extRoot,
    stdio: 'inherit',
});

if (result.status !== 0) {
    process.exit(result.status ?? 1);
}

// 规避 antlr-ng TS 目标未翻译 Java 风格语义谓词的 bug（详见 BUGS.md）。
// `{getCharPositionInLine()==0}?` 在 antlr4ng 运行时对应 `this.column`。
const lexerPath = join(outDir, 'yuxLexer.ts');
if (existsSync(lexerPath)) {
    const src = readFileSync(lexerPath, 'utf8');
    const patched = src.replace(/\bgetCharPositionInLine\(\)/g, 'this.column');
    if (patched !== src) {
        writeFileSync(lexerPath, patched, 'utf8');
        console.log(`[gen-parser] patched getCharPositionInLine() -> this.column in yuxLexer.ts`);
    }
}

console.log(`[gen-parser] ok -> ${outDir}`);
