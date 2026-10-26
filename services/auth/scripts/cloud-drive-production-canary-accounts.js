// Run on the MaiChat ECS as root. No action runs without an explicit mode.
'use strict';

const fs = require('node:fs');
const path = require('node:path');
const { randomBytes } = require('node:crypto');
const { parseEnv } = require('node:util');
const { execFileSync } = require('node:child_process');

const DIRECTORY = '/root/maichat-cloud-drive-canary';
const LEDGER = path.join(DIRECTORY, 'accounts.json');
const ENV_FILE = '/etc/maichat/auth.env';
const CANARY_ENV_FILE = '/etc/maichat/cloud-drive-auth-canary.env';
const SERVICE = 'maichat-auth.service';
const ADMIN = 'http://127.0.0.1:9101';
const AUTH = 'http://127.0.0.1:9100';
const ID_RE = /^cloudcanary_[ab]_[0-9a-f]{12}$/;
const MODE = process.argv[2];
let stage = 'start';

function newCredential(letter) {
  if (letter !== 'a' && letter !== 'b') throw Error('invalid canary label');
  return { id: `cloudcanary_${letter}_${randomBytes(6).toString('hex')}`,
    password: randomBytes(36).toString('base64url'), state: 'prepared' };
}

function allowlistMatchesPrimary(value, primaryId) {
  return typeof value === 'string' && value.trim() === primaryId;
}

function validateCanaryConfiguration(base, canary, live, loadedFiles, primaryId) {
  const files = loadedFiles.split(/\r?\n/).map(line => line.trim());
  if (!files.includes(`${ENV_FILE} (ignore_errors=no)`) ||
      !files.includes(`${CANARY_ENV_FILE} (ignore_errors=no)`))
    throw Error('auth service has not loaded both required environment files');
  if (Object.hasOwn(canary, 'MAICHAT_AUTH_ADMIN_SECRET') ||
      Object.hasOwn(canary, 'MAICHAT_AUTH_ADMIN_PORT') ||
      Object.hasOwn(canary, 'MAICHAT_AUTH_PORT'))
    throw Error('canary file must not override auth admin configuration');
  if (Number(base.MAICHAT_AUTH_ADMIN_PORT || 9101) !== 9101 ||
      Number(base.MAICHAT_AUTH_PORT || 9100) !== 9100 ||
      typeof base.MAICHAT_AUTH_ADMIN_SECRET !== 'string' ||
      base.MAICHAT_AUTH_ADMIN_SECRET.length < 32)
    throw Error('auth configuration unavailable');
  if (canary.MAICHAT_CLOUD_DRIVE_ENABLED !== '1' ||
      !allowlistMatchesPrimary(canary.MAICHAT_CLOUD_DRIVE_ALLOWED_ACCOUNTS,
        primaryId) ||
      typeof canary.MAICHAT_CLOUD_DRIVE_INTERNAL_KEY !== 'string' ||
      canary.MAICHAT_CLOUD_DRIVE_INTERNAL_KEY.length < 32)
    throw Error('cloud drive canary configuration unavailable');
  if (live.MAICHAT_CLOUD_DRIVE_ENABLED !== '1' ||
      !allowlistMatchesPrimary(live.MAICHAT_CLOUD_DRIVE_ALLOWED_ACCOUNTS,
        primaryId) ||
      live.MAICHAT_CLOUD_DRIVE_INTERNAL_KEY !==
        canary.MAICHAT_CLOUD_DRIVE_INTERNAL_KEY ||
      live.MAICHAT_AUTH_ADMIN_SECRET !== base.MAICHAT_AUTH_ADMIN_SECRET)
    throw Error('running auth service does not have the canary configuration');
  return { ...base, ...canary };
}

function requireRoot() {
  if (process.getuid?.() !== 0) throw Error('root required');
}
function requirePrivateFile(filename) {
  const stat = fs.lstatSync(filename);
  if (!stat.isFile() || stat.isSymbolicLink() || stat.uid !== 0 ||
      (stat.mode & 0o777) !== 0o600) throw Error('0600 root file required');
}
function requirePrivateDirectory() {
  fs.mkdirSync(DIRECTORY, { recursive: true, mode: 0o700 });
  const stat = fs.lstatSync(DIRECTORY);
  if (!stat.isDirectory() || stat.isSymbolicLink() || stat.uid !== 0 ||
      (stat.mode & 0o777) !== 0o700) throw Error('0700 root directory required');
}
function readLedger() {
  requirePrivateFile(LEDGER);
  const ledger = JSON.parse(fs.readFileSync(LEDGER, 'utf8'));
  const a = ledger.accounts?.A, b = ledger.accounts?.B;
  if (ledger.version !== 1 || !ID_RE.test(a?.id) || !ID_RE.test(b?.id) ||
      a.id === b.id || !a.id.startsWith('cloudcanary_a_') ||
      !b.id.startsWith('cloudcanary_b_') ||
      ![a,b].every(v => typeof v.password === 'string' &&
        v.password.length >= 32 && v.password.length <= 128 &&
        ['prepared','create_requested','created','verified'].includes(v.state)))
    throw Error('invalid account ledger');
  return ledger;
}
function writeLedger(ledger, first = false) {
  const body = JSON.stringify(ledger) + '\n';
  const temporary = first ? LEDGER : `${LEDGER}.tmp-${process.pid}`;
  const fd = fs.openSync(temporary, 'wx', 0o600);
  try { fs.writeFileSync(fd, body); fs.fsyncSync(fd); }
  finally { fs.closeSync(fd); }
  if (!first) fs.renameSync(temporary, LEDGER);
  requirePrivateFile(LEDGER);
}
function prepare() {
  stage = 'prepare';
  requirePrivateDirectory();
  if (fs.existsSync(LEDGER)) throw Error('account ledger already exists');
  writeLedger({ version: 1, created_at: new Date().toISOString(),
    accounts: { A: newCredential('a'), B: newCredential('b') } }, true);
  console.log('prepare=ok');
  console.log(`credential_file=${LEDGER}`);
}
function loadEnv(ledger) {
  stage = 'configuration';
  requirePrivateFile(ENV_FILE);
  requirePrivateFile(CANARY_ENV_FILE);
  const base = parseEnv(fs.readFileSync(ENV_FILE, 'utf8'));
  const canary = parseEnv(fs.readFileSync(CANARY_ENV_FILE, 'utf8'));
  const loadedFiles = execFileSync('systemctl',
    ['show','-P','EnvironmentFiles',SERVICE],
    { encoding:'utf8',timeout:5000,maxBuffer:4096 });
  const pid = Number(execFileSync('systemctl',['show','-P','MainPID',SERVICE],
    { encoding:'utf8',timeout:5000,maxBuffer:128 }).trim());
  if (!Number.isSafeInteger(pid) || pid <= 1)
    throw Error('auth service is not running');
  const live = {};
  for (const entry of fs.readFileSync(`/proc/${pid}/environ`,'utf8').split('\0')) {
    const equals = entry.indexOf('=');
    if (equals > 0) live[entry.slice(0,equals)] = entry.slice(equals+1);
  }
  return validateCanaryConfiguration(base,canary,live,loadedFiles,
    ledger.accounts.A.id);
}
async function request(url, options = {}) {
  return fetch(url, { redirect: 'error', signal: AbortSignal.timeout(30000),
    ...options });
}
async function adminRequest(route, secret, form) {
  const response = await request(ADMIN + route, { method: 'POST',
    headers: { Authorization: 'Basic ' + Buffer.from(`admin:${secret}`).toString('base64'),
      Origin: ADMIN, 'Content-Type': 'application/x-www-form-urlencoded' },
    body: new URLSearchParams(form) });
  await response.arrayBuffer();
  return response.status;
}
async function preflight(secret) {
  stage = 'preflight';
  const health = await request(AUTH + '/health');
  console.log(`auth_health_http=${health.status}`);
  if (health.status !== 200) throw Error('auth health failed');
  const page = await request(ADMIN + '/', { headers: {
    Authorization: 'Basic ' + Buffer.from(`admin:${secret}`).toString('base64') } });
  console.log(`admin_basic_http=${page.status}`);
  if (page.status !== 200) throw Error('admin authentication failed');
  await page.arrayBuffer();
}
async function login(account, optional = false) {
  const response = await request(AUTH + '/v1/auth/password/login', { method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ account_id: account.id, password: account.password,
      device_slot: 'mac', age_confirmed_14_plus: true }) });
  console.log(`account_${account.id.startsWith('cloudcanary_a_') ? 'A' : 'B'}_login_http=${response.status}`);
  if (optional && response.status === 401) return null;
  if (response.status !== 200) throw Error('password login failed');
  const body = await response.json();
  if (typeof body.access_token !== 'string') throw Error('login token missing');
  return body.access_token;
}
async function verify(account, label, token) {
  stage = `${label}_verify`;
  const headers = { Authorization: `Bearer ${token}` };
  const me = await request(AUTH + '/v1/auth/me', { headers });
  console.log(`account_${label}_auth_me_http=${me.status}`);
  if (me.status !== 200) throw Error('auth/me failed');
  const identity = await me.json();
  if (identity.user?.id !== account.id || identity.device_slot !== 'mac')
    throw Error('auth/me identity mismatch');
  const cloud = await request(AUTH + '/v1/cloud-drive/videos', { headers });
  console.log(`account_${label}_cloud_list_http=${cloud.status}`);
  if (label === 'A') {
    if (cloud.status !== 200) throw Error('cloud drive canary unavailable');
    const listing = await cloud.json();
    if (!Array.isArray(listing.videos) || listing.videos.length !== 0)
      throw Error('canary account unexpectedly has cloud videos');
  } else if (cloud.status !== 404) {
    throw Error('account B unexpectedly reached cloud drive');
  }
  const logout = await request(AUTH + '/v1/auth/logout', { method: 'POST', headers });
  console.log(`account_${label}_logout_http=${logout.status}`);
  if (logout.status !== 200) throw Error('logout failed');
}
async function provision() {
  stage = 'ledger';
  const ledger = readLedger();
  const env = loadEnv(ledger);
  await preflight(env.MAICHAT_AUTH_ADMIN_SECRET);
  for (const label of ['A','B']) {
    const account = ledger.accounts[label];
    stage = `${label}_create`;
    if (account.state === 'prepared') {
      account.state = 'create_requested';
      writeLedger(ledger);
      const status = await adminRequest('/create', env.MAICHAT_AUTH_ADMIN_SECRET,
        { account_id: account.id, password: account.password });
      console.log(`account_${label}_admin_create_http=${status}`);
      if (status !== 200) throw Error('admin create failed');
      account.state = 'created';
      writeLedger(ledger);
    }
    let token = await login(account, account.state === 'create_requested');
    if (!token) {
      const status = await adminRequest('/create', env.MAICHAT_AUTH_ADMIN_SECRET,
        { account_id: account.id, password: account.password });
      console.log(`account_${label}_admin_create_http=${status}`);
      if (status !== 200) throw Error('admin create failed');
      account.state = 'created';
      writeLedger(ledger);
      token = await login(account);
    }
    await verify(account, label, token);
    account.state = 'verified';
    writeLedger(ledger);
  }
  console.log('provision=verified');
}

async function main() {
  requireRoot();
  if (MODE === '--prepare' && process.argv.length === 3) prepare();
  else if (MODE === '--provision' && process.argv.length === 3) await provision();
  else throw Error('use --prepare or --provision');
}

if (require.main === module) {
  main().catch(() => {
    console.error(`canary_accounts_failed_stage=${stage}`);
    process.exitCode = 1;
  });
}

module.exports = { newCredential, allowlistMatchesPrimary,
  validateCanaryConfiguration };
