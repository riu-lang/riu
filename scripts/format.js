/*
 * Copyright (c) 2026. Yin-Jinlong@github
 * MPL-2.0
 *
 * C/C++ 源文件格式化包装器: 调 clang-format -i (含 #include 块内排序).
 *
 * 默认作用域: git 已变动 (含未跟踪) 的 .h / .hpp / .cpp / .cc 文件,
 *            排除 gen/ / third_party/ / build/ / .xmake/ / .cache/.
 * --all: 扫 src/、include/、sdk/yux/src/ 下所有匹配.
 * 显式文件位置参数: 只处理列出的文件.
 * --check: 只检查不改 (clang-format --dry-run -Werror), 有差异退出码 1.
 *
 * 用法:
 *   node scripts/format.js                 ; 改了什么格式化什么
 *   node scripts/format.js --all           ; 全仓
 *   node scripts/format.js src/foo.cpp ... ; 指定文件
 *   node scripts/format.js --check         ; 只检查不改
 */

const fs = require('fs');
const path = require('path');
const { execSync, spawnSync } = require('child_process');

const PROJECT_ROOT = path.join(__dirname, '..');
const SRC_EXTS = new Set(['.h', '.hpp', '.cpp', '.cc', '.cxx']);
const EXCLUDE_DIRS = [/^gen[\\/]/, /^third_party[\\/]/, /[\\/]build[\\/]/, /^build[\\/]/, /[\\/]\.xmake[\\/]/, /[\\/]\.cache[\\/]/];

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

function walkDir(dir, out) {
  let entries;
  try { entries = fs.readdirSync(dir, { withFileTypes: true }); } catch (_) { return; }
  for (const e of entries) {
    const full = path.join(dir, e.name);
    const rel = path.relative(PROJECT_ROOT, full).replace(/\\/g, '/');
    if (EXCLUDE_DIRS.some(re => re.test(rel.replace(/\//g, path.sep)))) continue;
    if (e.isDirectory()) walkDir(full, out);
    else out.push(full);
  }
}

function allRepoFiles() {
  const roots = ['src', 'include', 'sdk/yux/src'];
  const out = [];
  for (const r of roots) walkDir(path.join(PROJECT_ROOT, r), out);
  return out;
}

function filterCxx(files) {
  return files
    .map(f => path.isAbsolute(f) ? f : path.join(PROJECT_ROOT, f))
    .filter(f => SRC_EXTS.has(path.extname(f).toLowerCase()))
    .filter(f => {
      const rel = path.relative(PROJECT_ROOT, f).replace(/\\/g, path.sep);
      return !EXCLUDE_DIRS.some(re => re.test(rel));
    })
    .filter(f => fs.existsSync(f));
}

function findClangFormat() {
  const which = process.platform === 'win32' ? 'where' : 'which';
  try {
    const out = execSync(`${which} clang-format`, { encoding: 'utf8' });
    const first = out.split(/\r?\n/).find(l => l.trim());
    if (first && fs.existsSync(first.trim())) return first.trim();
  } catch (_) { /* fallthrough */ }
  return null;
}

function runClangFormat(clang, files, check) {
  // 分批跑, 避免命令行过长 (Windows ~8K).
  const BATCH = 50;
  let failed = 0;
  for (let i = 0; i < files.length; i += BATCH) {
    const batch = files.slice(i, i + BATCH);
    const args = check ? ['--dry-run', '-Werror', ...batch] : ['-i', ...batch];
    const r = spawnSync(clang, args, { cwd: PROJECT_ROOT, encoding: 'utf8' });
    if (r.status !== 0) {
      failed++;
      if (r.stderr) process.stderr.write(r.stderr);
      if (r.stdout) process.stdout.write(r.stdout);
    }
  }
  return failed;
}

function main() {
  const args = process.argv.slice(2);
  const flagAll = args.includes('--all');
  const flagCheck = args.includes('--check');
  const positional = args.filter(a => !a.startsWith('--'));

  let candidates;
  if (positional.length > 0) {
    candidates = positional;
    log(`scope: ${positional.length} explicit file(s)`, 'cyan');
  } else if (flagAll) {
    candidates = allRepoFiles().map(f => path.relative(PROJECT_ROOT, f));
    log(`scope: --all (${candidates.length} candidate file(s))`, 'cyan');
  } else {
    candidates = gitChangedFiles();
    log(`scope: git changed (${candidates.length} candidate file(s))`, 'cyan');
  }

  const files = filterCxx(candidates);
  if (files.length === 0) {
    log('nothing to do (no matching C/C++ source files).', 'gray');
    return;
  }

  const clang = findClangFormat();
  if (!clang) {
    log('clang-format not found in PATH; aborting.', 'red');
    process.exit(2);
  }
  log(`clang-format: ${clang}`, 'gray');

  const failed = runClangFormat(clang, files, flagCheck);

  if (flagCheck) {
    if (failed > 0) {
      log(`\n${failed} batch(es) report format diff. Run \`./format.cmd\` to fix.`, 'red');
      process.exit(1);
    }
    log(`\nall ${files.length} file(s) match clang-format style.`, 'green');
  } else {
    log(`\nformatted ${files.length} file(s).`, 'green');
  }
}

main();
