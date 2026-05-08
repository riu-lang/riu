/*
 * Copyright (c) 2026. Yin-Jinlong@github
 * MPL-2.0
 * 
 * 生成跨平台包装脚本（.ps1, .sh, .cmd）
 * 在项目根目录运行：node init.js
 */

const fs = require('fs');
const path = require('path');

const PROJECT_ROOT = __dirname;
const SCRIPTS_DIR = path.join(__dirname, 'scripts');

const SCRIPTS = ['sync-deps', 'gen-antlr', 'count-lines'];

const TEMPLATES = {
  ps1: (name) => `#!/usr/bin/env pwsh
$scriptDir = $PSScriptRoot
& node "$scriptDir/scripts/${name}.js" @args
`,
  sh: (name) => `#!/bin/sh
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
exec node "$SCRIPT_DIR/scripts/${name}.js" "$@"
`,
  cmd: (name) => `@echo off
node "%~dp0scripts\\${name}.js" %*
`
};

function log(msg, color = 'reset') {
  const colors = {
    reset: '\x1b[0m',
    cyan: '\x1b[36m',
    green: '\x1b[32m',
    yellow: '\x1b[33m',
    red: '\x1b[31m'
  };
  console.log(`${colors[color] || ''}${msg}${colors.reset}`);
}

function generateScripts() {
  log('\n=== Generating wrapper scripts ===\n', 'cyan');

  for (const name of SCRIPTS) {
    log(`Generating scripts for: ${name}`, 'cyan');
    
    const ps1Path = path.join(PROJECT_ROOT, `${name}.ps1`);
    const shPath = path.join(PROJECT_ROOT, `${name}.sh`);
    const cmdPath = path.join(PROJECT_ROOT, `${name}.cmd`);
    
    fs.writeFileSync(ps1Path, TEMPLATES.ps1(name));
    log(`  Created: ${name}.ps1`, 'green');
    
    fs.writeFileSync(shPath, TEMPLATES.sh(name));
    log(`  Created: ${name}.sh`, 'green');
    
    fs.writeFileSync(cmdPath, TEMPLATES.cmd(name));
    log(`  Created: ${name}.cmd`, 'green');
  }

  log('\n=== Done ===', 'green');
  log('\nGenerated scripts:', 'cyan');
  for (const name of SCRIPTS) {
    log(`  ${name}.ps1, ${name}.sh, ${name}.cmd`);
  }
}

generateScripts();
