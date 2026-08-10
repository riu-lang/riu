/*
 * Copyright (c) 2026. Yin-Jinlong@github
 * MPL-2.0
 */

const fs = require('fs');
const path = require('path');
const { execSync, exec } = require('child_process');

const PROJECT_ROOT = path.join(__dirname, '..');
const DEPS_FILE = path.join(PROJECT_ROOT, 'DEPS.json');
const THIRD_PARTY_DIR = path.join(PROJECT_ROOT, 'third_party');
const BIN_DIR = path.join(PROJECT_ROOT, 'bin');
const SDK_SRC_DIR = path.join(PROJECT_ROOT, 'sdk');
const SDK_LINK_DIR = path.join(PROJECT_ROOT, 'build', 'windows', 'x64', 'sdk');

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


async function downloadFile(url, destPath) {
  const https = require('https');
  const http = require('http');
  
  return new Promise((resolve, reject) => {
    const protocol = url.startsWith('https') ? https : http;
    
    log(`Downloading ${url}...`);
    
    const file = fs.createWriteStream(destPath);
    
    protocol.get(url, (response) => {
      if (response.statusCode === 301 || response.statusCode === 302) {
        file.close();
        fs.unlinkSync(destPath);
        downloadFile(response.headers.location, destPath).then(resolve).catch(reject);
        return;
      }
      
      if (response.statusCode !== 200) {
        file.close();
        fs.unlinkSync(destPath);
        reject(new Error(`HTTP ${response.statusCode}`));
        return;
      }
      
      response.pipe(file);
      
      file.on('finish', () => {
        file.close();
        resolve();
      });
    }).on('error', (err) => {
      file.close();
      fs.unlinkSync(destPath);
      reject(err);
    });
  });
}

async function syncBinary(name, config) {
  const { url, version } = config;
  const targetPath = path.join(BIN_DIR, path.basename(url));
  
  log(`\n=== Syncing binary ${name} ===`, 'cyan');
  log(`URL: ${url}`);
  log(`Version: ${version}`);
  log(`Target: ${targetPath}`);
  
  fs.mkdirSync(BIN_DIR, { recursive: true });
  
  if (fs.existsSync(targetPath)) {
    log(`Already exists, skipping`, 'green');
    return;
  }
  
  try {
    await downloadFile(url, targetPath);
    log(`Downloaded ${name}`, 'green');
  } catch (e) {
    log(`Failed to download ${name}: ${e.message}`, 'red');
    throw e;
  }
}

function syncSdkLink() {
  log(`\n=== Linking SDK ===`, 'cyan');
  log(`Source: ${SDK_SRC_DIR}`);
  log(`Link:   ${SDK_LINK_DIR}`);

  if (!fs.existsSync(SDK_SRC_DIR)) {
    log(`SDK source missing: ${SDK_SRC_DIR}`, 'red');
    throw new Error('sdk/ not found at project root');
  }

  fs.mkdirSync(path.dirname(SDK_LINK_DIR), { recursive: true });

  let st = null;
  try { st = fs.lstatSync(SDK_LINK_DIR); } catch {}
  if (st) {
    if (st.isSymbolicLink()) {
      try {
        const cur = fs.readlinkSync(SDK_LINK_DIR);
        const resolved = path.resolve(path.dirname(SDK_LINK_DIR), cur);
        if (resolved === SDK_SRC_DIR) {
          log(`Already linked, skipping`, 'green');
          return;
        }
      } catch {}
      fs.unlinkSync(SDK_LINK_DIR);
    } else {
      fs.rmSync(SDK_LINK_DIR, { recursive: true, force: true });
    }
  }

  const type = process.platform === 'win32' ? 'junction' : 'dir';
  fs.symlinkSync(SDK_SRC_DIR, SDK_LINK_DIR, type);
  log(`Linked (${type})`, 'green');
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
  const binaries = deps.binaries || {};
  const dependencies = deps.dependencies || {};

  fs.mkdirSync(THIRD_PARTY_DIR, { recursive: true });
  fs.mkdirSync(BIN_DIR, { recursive: true });
  const binariesToProcess = targetDeps.length > 0
    ? Object.fromEntries(targetDeps.filter(n => binaries[n]).map(n => [n, binaries[n]]))
    : binaries;

  const toProcess = targetDeps.length > 0
    ? Object.fromEntries(targetDeps.filter(n => dependencies[n]).map(n => [n, dependencies[n]]))
    : Object.fromEntries(Object.entries(dependencies));

  if (Object.keys(binariesToProcess).length === 0 && Object.keys(toProcess).length === 0) {
    log('No dependencies to process', 'yellow');
    return;
  }

  log(`\nSyncing ${Object.keys(binariesToProcess).length} binaries...`, 'cyan');
  log(`Syncing ${Object.keys(toProcess).length} dependencies...`, 'cyan');

  if (dryRun) {
    log('(dry run)', 'yellow');
    for (const [name, config] of Object.entries(binariesToProcess)) {
      log(`\n  binary/${name}: ${config.version}`);
    }
    for (const [name, config] of Object.entries(toProcess)) {
      log(`\n  ${name}: ${config.commit.substring(0, 8)}`);
    }
    return;
  }

  for (const [name, config] of Object.entries(binariesToProcess)) {
    try {
      await syncBinary(name, config);
    } catch (e) {
      log(`Failed to sync binary ${name}: ${e.message}`, 'red');
      process.exit(1);
    }
  }

  for (const [name, config] of Object.entries(toProcess)) {
    try {
      await syncDependency(name, config);
    } catch (e) {
      log(`Failed to sync ${name}: ${e.message}`, 'red');
      process.exit(1);
    }
  }


  if (targetDeps.length === 0) {
    try {
      syncSdkLink();
    } catch (e) {
      log(`Failed to link SDK: ${e.message}`, 'red');
      process.exit(1);
    }
  }

  log('\n=== Done ===', 'green');
}

main().catch(e => {
  log(`Error: ${e.message}`, 'red');
  process.exit(1);
});
