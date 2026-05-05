/*
 * Copyright (c) 2026. Yin-Jinlong@github
 * MPL-2.0
 * 
 * 使用 ANTLR4 jar 从 yuxLexer.g4 / yuxParser.g4 生成 C++ 解析器代码
 */

const fs = require('fs');
const path = require('path');
const { execSync } = require('child_process');

const PROJECT_ROOT = path.join(__dirname, '..');
const LEXER_GRAMMAR = path.join(PROJECT_ROOT, 'src', 'yuxLexer.g4');
const PARSER_GRAMMAR = path.join(PROJECT_ROOT, 'src', 'yuxParser.g4');
const OUTPUT_DIR = path.join(PROJECT_ROOT, 'gen', 'yux');
const JAR_FILE = path.join(PROJECT_ROOT, 'bin', 'antlr-4.13.2-complete.jar');

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

function main() {
  log('\n=== ANTLR4 C++ Code Generator ===\n', 'cyan');

  if (!fs.existsSync(JAR_FILE)) {
    log(`ANTLR4 jar not found: ${JAR_FILE}`, 'red');
    log('Run sync-deps first to download the jar file.', 'yellow');
    process.exit(1);
  }

  for (const g of [LEXER_GRAMMAR, PARSER_GRAMMAR]) {
    if (!fs.existsSync(g)) {
      log(`Grammar file not found: ${g}`, 'red');
      process.exit(1);
    }
  }

  fs.mkdirSync(OUTPUT_DIR, { recursive: true });

  log(`Lexer:  ${LEXER_GRAMMAR}`);
  log(`Parser: ${PARSER_GRAMMAR}`);
  log(`Output: ${OUTPUT_DIR}`);
  log(`JAR:    ${JAR_FILE}\n`);

  // Lexer 必须先生成，parser 通过 tokenVocab 引用 yuxLexer.tokens
  for (const g of [LEXER_GRAMMAR, PARSER_GRAMMAR]) {
    const cmd = `java -jar "${JAR_FILE}" -Dlanguage=Cpp -package yux -visitor -no-listener -o "${OUTPUT_DIR}" -lib "${OUTPUT_DIR}" "${g}"`;
    log(`Running: ${cmd}\n`);
    try {
      execSync(cmd, { stdio: 'inherit' });
    } catch (e) {
      log(`\nGeneration failed for ${g}!`, 'red');
      process.exit(1);
    }
  }

  log('\nGeneration completed successfully!', 'green');
  const files = fs.readdirSync(OUTPUT_DIR).filter(f => f.endsWith('.cpp') || f.endsWith('.h'));
  log(`\nGenerated ${files.length} files:`, 'cyan');
  files.forEach(f => log(`  - ${f}`));
}

main();
