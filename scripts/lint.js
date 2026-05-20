/*
 * Copyright (c) 2026. Yin-Jinlong@github
 * MPL-2.0
 *
 * clang-tidy 包装（基于 xmake check clang.tidy）。
 *
 * 默认作用域：git 已变动（含未跟踪）的 .h / .hpp / .cpp / .cc 文件，
 *            排除 gen/ / third_party/ / build/。
 * 可选 --all：扫 yux_frontend / yux_codegen / yux 三个 target 全量。
 * 可选 显式文件位置参数：只对列出的文件跑（透传 -f）。
 *
 * 用法：
 *   node scripts/lint.js                 ; 改了什么 lint 什么
 *   node scripts/lint.js --all           ; 三个 target 全量
 *   node scripts/lint.js src/foo.cpp ... ; 指定文件
 */

const fs = require('fs');
const path = require('path');
const { execSync, spawnSync } = require('child_process');

const PROJECT_ROOT = path.join(__dirname, '..');
const TARGETS = ['yux_frontend', 'yux_codegen', 'yux'];
const SRC_EXTS = new Set(['.h', '.hpp', '.cpp', '.cc', '.cxx']);
const EXCLUDE_RE = /(^|[\\/])(gen|third_party|build|\.xmake|\.cache)([\\/]|$)/;

function log(msg, color = 'reset') {
  const colors = { reset: '\x1b[0m', cyan: '\x1b[36m', green: '\x1b[32m', yellow: '\x1b[33m', red: '\x1b[31m', gray: '\x1b[90m' };
  console.log(`${colors[color] || ''}${msg}${colors.reset}`);
}

function gitChangedFiles() {
  const out = execSync('git status --porcelain --untracked-files=all', { cwd: PROJECT_ROOT, encoding: 'utf8' });
  const files = [];
  for (const line of out.split(/\r?\n/)) {
    if (!line) continue;
    const status = line.slice(0, 2);
    let rest = line.slice(3);
    if (status.startsWith('R') || status.startsWith('C')) {
      const arrow = rest.indexOf(' -> ');
      if (arrow >= 0) rest = rest.slice(arrow + 4);
    }
    if (status[0] === 'D' || status[1] === 'D') continue;
    if (rest.startsWith('"') && rest.endsWith('"')) {
      try { rest = JSON.parse(rest); } catch (_) { rest = rest.slice(1, -1); }
    }
    files.push(rest);
  }
  return files;
}

function filterCxx(files) {
  return files
    .map(f => path.isAbsolute(f) ? path.relative(PROJECT_ROOT, f) : f)
    .map(f => f.replace(/\\/g, '/'))
    .filter(f => SRC_EXTS.has(path.extname(f).toLowerCase()))
    .filter(f => !EXCLUDE_RE.test(f.replace(/\//g, path.sep)))
    .filter(f => fs.existsSync(path.join(PROJECT_ROOT, f)));
}

function main() {
  const args = process.argv.slice(2);
  const flagAll = args.includes('--all');
  const positional = args.filter(a => !a.startsWith('--'));

  let xmakeArgs;
  if (positional.length > 0) {
    const files = filterCxx(positional);
    if (files.length === 0) { log('no matching files.', 'gray'); return; }
    log(`scope: ${files.length} explicit file(s)`, 'cyan');
    xmakeArgs = ['check', 'clang.tidy', '-f', files.join(';')];
  } else if (flagAll) {
    log(`scope: --all (targets: ${TARGETS.join(' ')})`, 'cyan');
    xmakeArgs = ['check', 'clang.tidy', ...TARGETS];
  } else {
    const files = filterCxx(gitChangedFiles());
    if (files.length === 0) { log('no git-changed C/C++ files; nothing to lint.', 'gray'); return; }
    log(`scope: git changed (${files.length} file(s))`, 'cyan');
    xmakeArgs = ['check', 'clang.tidy', '-f', files.join(';')];
  }

  log(`\n=== xmake ${xmakeArgs.join(' ')} ===\n`, 'cyan');
  const r = spawnSync('xmake', xmakeArgs, { cwd: PROJECT_ROOT, stdio: 'inherit', shell: true });
  if (r.status !== 0) {
    log(`\nclang-tidy exited with status ${r.status}`, 'red');
    process.exit(r.status || 1);
  }
  log('\nclang-tidy done.', 'green');
}

main();
