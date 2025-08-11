/*
 * Copyright (c) 2026. Yin-Jinlong@github
 */

const fs = require('fs');
const path = require('path');
const { execSync, exec } = require('child_process');

const DEPS_FILE = path.join(__dirname, 'DEPS.json');
const THIRD_PARTY_DIR = path.join(__dirname, 'third_party');

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

function run(cmd, cwd = process.cwd()) {
  try {
    return execSync(cmd, { cwd, encoding: 'utf8', stdio: ['pipe', 'pipe', 'pipe'] }).trim();
  } catch (e) {
    return null;
  }
}

function runAsync(cmd, cwd) {
  return new Promise((resolve, reject) => {
    exec(cmd, { cwd, encoding: 'utf8' }, (error, stdout, stderr) => {
      if (error) reject(new Error(stderr || error.message));
      else resolve(stdout.trim());
    });
  });
}

function getRepoCommit(repoPath) {
  return run('git rev-parse HEAD', repoPath);
}

function isShallow(repoPath) {
  const shallowFile = path.join(repoPath, '.git', 'shallow');
  return fs.existsSync(shallowFile);
}

async function cloneRepo(name, url, commit, targetPath) {
  log(`\n=== Cloning ${name} ===`, 'cyan');
  log(`URL: ${url}`);
  log(`Commit: ${commit.substring(0, 8)}...`);

  if (fs.existsSync(targetPath)) {
    fs.rmSync(targetPath, { recursive: true, force: true });
  }

  fs.mkdirSync(path.dirname(targetPath), { recursive: true });

  log('Cloning (shallow, no checkout)...');
  await runAsync(`git clone --depth 1 --no-checkout "${url}" "${targetPath}"`);

  log(`Fetching commit ${commit.substring(0, 8)}...`);
  await runAsync(`git fetch --depth 1 origin ${commit}`, targetPath);

  log('Checking out...');
  await runAsync(`git checkout FETCH_HEAD`, targetPath);

  log(`Cloned ${name}`, 'green');
}

async function updateRepo(name, url, commit, targetPath) {
  log(`\n=== Updating ${name} ===`, 'cyan');
  log(`URL: ${url}`);
  log(`Target: ${commit.substring(0, 8)}...`);

  const currentCommit = getRepoCommit(targetPath);
  if (currentCommit === commit) {
    log(`Already at ${commit.substring(0, 8)}, skipping`, 'green');
    return;
  }

  log(`Current: ${currentCommit ? currentCommit.substring(0, 8) : 'unknown'}`);
  log(`Updating...`);

  try {
    await runAsync(`git fetch --depth 1 origin ${commit}`, targetPath);
    await runAsync(`git checkout FETCH_HEAD`, targetPath);
    log(`Updated ${name}`, 'green');
  } catch (e) {
    log(`Fetch failed, re-cloning...`, 'yellow');
    await cloneRepo(name, url, commit, targetPath);
  }
}

async function syncDependency(name, config) {
  const { url, commit } = config;
  const targetPath = path.join(THIRD_PARTY_DIR, name);

  if (!fs.existsSync(targetPath)) {
    await cloneRepo(name, url, commit, targetPath);
    return;
  }

  if (!fs.existsSync(path.join(targetPath, '.git'))) {
    log(`\n${name} exists but is not a git repo, re-cloning...`, 'yellow');
    await cloneRepo(name, url, commit, targetPath);
    return;
  }

  await updateRepo(name, url, commit, targetPath);
}

async function main() {
  const args = process.argv.slice(2);
  const dryRun = args.includes('--dry-run') || args.includes('-n');
  const targetDeps = args.filter(a => !a.startsWith('-'));

  if (!fs.existsSync(DEPS_FILE)) {
    log('DEPS.json not found', 'red');
    process.exit(1);
  }

  const deps = JSON.parse(fs.readFileSync(DEPS_FILE, 'utf8'));
  const dependencies = deps.dependencies || {};

  fs.mkdirSync(THIRD_PARTY_DIR, { recursive: true });

  const toProcess = targetDeps.length > 0
    ? Object.fromEntries(targetDeps.filter(n => dependencies[n]).map(n => [n, dependencies[n]]))
    : dependencies;

  if (Object.keys(toProcess).length === 0) {
    log('No dependencies to process', 'yellow');
    return;
  }

  log(`\nSyncing ${Object.keys(toProcess).length} dependencies...`, 'cyan');

  if (dryRun) {
    log('(dry run)', 'yellow');
    for (const [name, config] of Object.entries(toProcess)) {
      log(`\n  ${name}: ${config.commit.substring(0, 8)}`);
    }
    return;
  }

  for (const [name, config] of Object.entries(toProcess)) {
    try {
      await syncDependency(name, config);
    } catch (e) {
      log(`Failed to sync ${name}: ${e.message}`, 'red');
      process.exit(1);
    }
  }

  log('\n=== Done ===', 'green');
}

main().catch(e => {
  log(`Error: ${e.message}`, 'red');
  process.exit(1);
});
