/*
 * Copyright (c) 2026. Yin-Jinlong@github
 * MPL-2.0
 *
 * #include 块内排序器（块 = 连续 #include 行，空行或非 #include 行打断块）。
 *
 * 默认作用域：git 已变动（含未跟踪）的 .h / .hpp / .cpp / .cc 文件，
 *            排除 gen/ / third_party/ / build/ / .xmake/ / .cache/。
 * 可选 --all：扫 src/、include/、sdk/yux/src/、tests/cases/、tests/projects/ 下所有匹配。
 * 可选 显式文件位置参数：只处理列出的文件。
 *
 * 用法：
 *   node scripts/format.js                 ; 改了什么排什么
 *   node scripts/format.js --all           ; 全仓
 *   node scripts/format.js src/foo.cpp ... ; 指定文件
 *   node scripts/format.js --check         ; 只检查不改, 有差异退出码 1
 */

const fs = require('fs');
const path = require('path');
const { execSync } = require('child_process');

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
    // porcelain: XY <space> path  (rename: XY <space> orig -> dest)
    const status = line.slice(0, 2);
    let rest = line.slice(3);
    if (status.startsWith('R') || status.startsWith('C')) {
      const arrow = rest.indexOf(' -> ');
      if (arrow >= 0) rest = rest.slice(arrow + 4);
    }
    // 删除的文件跳过
    if (status[0] === 'D' || status[1] === 'D') continue;
    // 去引号
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

function extractIncludeKey(line) {
  // #include <foo/bar.h>  /  #include "foo/bar.h"
  const m = line.match(/#\s*include\s*[<"]([^>"]+)[>"]/);
  return m ? m[1].toLowerCase() : line.toLowerCase();
}

function isIncludeLine(line) {
  return /^\s*#\s*include\s*[<"]/.test(line);
}

function sortIncludes(text) {
  // 保留行尾换行风格
  const lines = text.split(/\r?\n/);
  const out = [];
  let i = 0;
  let changed = false;
  while (i < lines.length) {
    if (isIncludeLine(lines[i])) {
      const block = [];
      while (i < lines.length && isIncludeLine(lines[i])) {
        block.push(lines[i]);
        i++;
      }
      const sorted = block.slice().sort((a, b) => extractIncludeKey(a).localeCompare(extractIncludeKey(b)));
      if (!changed) {
        for (let j = 0; j < block.length; j++) {
          if (block[j] !== sorted[j]) { changed = true; break; }
        }
      }
      out.push(...sorted);
    } else {
      out.push(lines[i]);
      i++;
    }
  }
  return { text: out.join('\n'), changed };
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

  let modified = 0;
  let mismatch = 0;
  for (const f of files) {
    const orig = fs.readFileSync(f, 'utf8');
    const { text, changed } = sortIncludes(orig);
    if (!changed) continue;
    const rel = path.relative(PROJECT_ROOT, f).replace(/\\/g, '/');
    if (flagCheck) {
      log(`  needs sort: ${rel}`, 'yellow');
      mismatch++;
    } else {
      fs.writeFileSync(f, text);
      log(`  sorted: ${rel}`, 'green');
      modified++;
    }
  }

  if (flagCheck) {
    if (mismatch > 0) {
      log(`\n${mismatch} file(s) need #include sort.`, 'red');
      process.exit(1);
    }
    log('\nall files have sorted #include blocks.', 'green');
  } else {
    log(`\n${modified} file(s) modified.`, modified > 0 ? 'green' : 'gray');
  }
}

main();
