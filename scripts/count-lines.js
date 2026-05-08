/*
 * Copyright (c) 2026. Yin-Jinlong@github
 * MPL-2.0
 * 
 * 使用 cloc 统计 git 仓库中的代码行数
 * 默认统计 HEAD，可指定 commit hash
 */

const { spawn } = require('child_process');
const path = require('path');

const commitHash = process.argv[2] || 'HEAD';
const repoRoot = path.resolve(__dirname, '..');
const langDefFile = path.join(repoRoot, 'yux_lang_def.txt');

const cloc = spawn('cloc', [
    commitHash,
    '--read-lang-def', langDefFile,
    '--not-match-f', 'lock\\.'
], {
    stdio: 'inherit'
});

cloc.on('error', (err) => {
    console.error('Failed to execute cloc:', err.message);
    process.exit(1);
});

cloc.on('close', (code) => {
    process.exit(code || 0);
});
