/*
 * Copyright (c) 2026. Yin-Jinlong@github
 * MPL-2.0
 *
 * P0 冒烟：用 esbuild 把 src/parser.ts + src/gen/** 现场打包到 tmp，
 * 解析 examples/main/main.yux，打印顶层规则子节点数与错误。
 * 通过即证明生成器 / 运行时 / TS 配置全部联通。
 * 注：visitor.ts 依赖 vscode runtime，其正确性留给 VSCode test host（P4）。
 */

import * as esbuild from 'esbuild';
import { readFileSync, mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const __dirname = dirname(fileURLToPath(import.meta.url));
const extRoot = resolve(__dirname, '..');
const repoRoot = resolve(extRoot, '..');

const tmpDir = mkdtempSync(join(tmpdir(), 'yux-smoke-'));
const outFile = join(tmpDir, 'parser.mjs');

try {
    await esbuild.build({
        entryPoints: [join(extRoot, 'src', 'parser.ts')],
        bundle: true,
        format: 'esm',
        platform: 'node',
        target: 'es2022',
        outfile: outFile,
        logLevel: 'warning',
    });

    const { parseYux } = await import(pathToFileURL(outFile).href);

    const sample = readFileSync(join(repoRoot, 'examples', 'main', 'main.yux'), 'utf8');
    const result = parseYux(sample);

    const topChildren = result.tree.children?.length ?? 0;
    console.log(`[smoke] parsed main.yux: topChildren=${topChildren}, errors=${result.errors.length}`);
    if (result.errors.length > 0) {
        for (const e of result.errors.slice(0, 5)) {
            console.log(`  ${e.line}:${e.column} ${e.message}`);
        }
        process.exit(1);
    }
    console.log('[smoke] ok');
} finally {
    rmSync(tmpDir, { recursive: true, force: true });
}
