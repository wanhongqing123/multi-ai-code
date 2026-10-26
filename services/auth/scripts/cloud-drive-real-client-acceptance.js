// Operator-run acceptance of the shared C++ client against the dedicated real OSS bucket.
// Production auth users, databases, flags, and listeners are never used.
'use strict';

const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const fs = require('node:fs/promises');
const fsSync = require('node:fs');
const http = require('node:http');
const https = require('node:https');
const path = require('node:path');
const { spawn, spawnSync } = require('node:child_process');
const { openStore } = require('../store');
const { createAuthService } = require('../service');
const { createServer: createAuthServer, cloudDriveFromEnv } = require('../server');
const { createHandler: createSignerHandler } = require('../../oss-media-signer/index');
const { createServer: createSignerServer } = require('../../oss-media-signer/server');
const { readServerCredentials, ensureRecoveryDir, writeRecovery, clearRecovery } =
  require('../../oss-media-signer/scripts/cloud-drive-oss-smoke');

const BUCKET = 'maichat-private-cloud-drive-gz-20261025-f346b709';
const REGION = 'oss-cn-guangzhou';
const BYTES = 2_000_000_000;
const PART_BYTES = 8 * 1024 * 1024;
const RECOVERY_DIR = '/var/lib/maichat/oss-smoke-recovery';
const WORK_ROOT = '/var/lib/maichat/cloud-real-acceptance/work';
const ID_PATTERN = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/;
let auditStage = 'startup';

function fail(message) { throw new Error(message); }
function parseArgs(argv) {
  if (argv.length === 1 && argv[0] === '--help') return null;
  const values = {};
  let ossInternal = false;
  for (const arg of argv) {
    if (arg === '--oss-internal') {
      if (ossInternal) fail('Invalid acceptance arguments');
      ossInternal = true;
      continue;
    }
    const match = /^--(env-file|client-binary|system-ca|execute)=(.+)$/.exec(arg);
    if (!match || values[match[1]]) fail('Invalid acceptance arguments');
    values[match[1]] = match[2];
  }
  if (Object.keys(values).length !== 4 || values.execute !== BUCKET ||
      !Object.values(values).every(Boolean))
    fail('Explicit dedicated bucket, credential file, binary and system CA are required');
  if (ossInternal) values.ossInternal = true;
  return values;
}

function ossHostname(internal = false) {
  return `${BUCKET}.${REGION}${internal ? '-internal' : ''}.aliyuncs.com`;
}

async function listen(server) {
  await new Promise((resolve,reject) => {
    server.once('error',reject);
    server.listen(0,'127.0.0.1',resolve);
  });
  return server.address().port;
}

async function close(server) {
  if (server?.listening) await new Promise(resolve => server.close(resolve));
}

async function preflight(storage) {
  const [acl,versioning,lifecycle] = await Promise.all([
    storage.getBucketACL(BUCKET), storage.getBucketVersioning(BUCKET),
    storage.getBucketLifecycle(BUCKET)
  ]);
  if (acl.acl !== 'private' ||
      (versioning.versionStatus !== undefined &&
       versioning.versionStatus !== 'Disabled'))
    fail('Dedicated bucket ACL or versioning is unsafe');
  const rule = (lifecycle.rules || []).find(item => item.status === 'Enabled' &&
    item.prefix === 'cloud-drive/' && Number(item.abortMultipartUpload?.days) === 8);
  const shorter = (lifecycle.rules || []).some(item => item.status === 'Enabled' &&
    typeof item.prefix === 'string' && 'cloud-drive/'.startsWith(item.prefix) &&
    item.abortMultipartUpload && Number(item.abortMultipartUpload.days) < 8);
  if (!rule || shorter) fail('Dedicated bucket lacks the 8-day multipart abort rule');
}

async function makeCertificate(directory, systemCA) {
  const key = path.join(directory,'gateway.key');
  const cert = path.join(directory,'gateway.pem');
  const bundle = path.join(directory,'ca-bundle.pem');
  const generated = spawnSync('openssl',['req','-x509','-newkey','rsa:2048',
    '-sha256','-nodes','-days','1','-subj','/CN=localhost',
    '-addext','subjectAltName=DNS:localhost,IP:127.0.0.1',
    '-keyout',key,'-out',cert],{ stdio:'ignore' });
  if (generated.status !== 0) fail('Could not make temporary loopback TLS certificate');
  await fs.chmod(key,0o600);
  await fs.writeFile(bundle,Buffer.concat([
    await fs.readFile(cert),Buffer.from('\n'),await fs.readFile(systemCA)]),
  { mode:0o600 });
  return { key:await fs.readFile(key), cert:await fs.readFile(cert), bundle };
}

function gateway(authPort,certificate) {
  return https.createServer(certificate,(incoming,outgoing) => {
    const upstream = http.request({ host:'127.0.0.1',port:authPort,
      method:incoming.method,path:incoming.url,headers:incoming.headers }, response => {
      outgoing.writeHead(response.statusCode,response.headers);
      response.pipe(outgoing);
    });
    upstream.on('error',() => {
      if (!outgoing.headersSent) outgoing.writeHead(502);
      outgoing.end();
    });
    incoming.pipe(upstream);
  });
}

async function call(base,token,method,route,body) {
  const response = await fetch(`${base}${route}`,{ method,redirect:'error',
    headers:{ Authorization:`Bearer ${token}`,'Content-Type':'application/json' },
    ...(body ? { body:JSON.stringify(body) } : {}) });
  let json;
  try { json = await response.json(); } catch { fail('Account service returned invalid JSON'); }
  return { status:response.status,body:json };
}

async function login(base,account,password) {
  const response = await fetch(`${base}/v1/auth/password/login`,{
    method:'POST',headers:{ 'Content-Type':'application/json' },
    body:JSON.stringify({ account_id:account,password,device_slot:'ios_phone',
      age_confirmed_14_plus:true })
  });
  if (response.status !== 200) fail('Disposable account login failed');
  const body = await response.json();
  if (typeof body.access_token !== 'string') fail('Disposable login had no token');
  return body.access_token;
}

async function runClient(binary,phase,settings) {
  const env = { PATH:process.env.PATH || '/usr/bin:/bin',
    MAICHAT_CLOUD_ACCEPT_AUTH_BASE:settings.authBase,
    MAICHAT_CLOUD_ACCEPT_TOKEN:settings.token,
    MAICHAT_CLOUD_ACCEPT_CA_BUNDLE:settings.ca,
    MAICHAT_CLOUD_ACCEPT_SOURCE:settings.source,
    MAICHAT_CLOUD_ACCEPT_CHECKPOINT:settings.checkpoint };
  await new Promise((resolve,reject) => {
    const child = spawn(binary,[phase],{ env,stdio:'ignore' });
    child.once('error',() => reject(new Error('Could not start C++ cloud client')));
    child.once('exit',(code,signal) => code === 0 ? resolve() :
      reject(new Error(`C++ cloud client ${phase} failed: exit=${code}, signal=${signal || 'none'}`)));
  });
}

function testPrefix(account) {
  if (!/^cloudtest_[0-9a-f]{12}$/.test(account)) fail('Invalid disposable test account');
  return `cloud-drive/${account}/`;
}

async function residual(storage,account) {
  const prefix = testPrefix(account);
  const [objectPage,uploadPage] = await Promise.all([
    storage.list({ prefix,'max-keys':10 }),
    storage.listUploads({ prefix,'max-uploads':10 })
  ]);
  const objects = objectPage.objects || [];
  const uploads = uploadPage.uploads || [];
  if (objectPage.isTruncated === true || objectPage.isTruncated === 'true' ||
      uploadPage.isTruncated === true || uploadPage.isTruncated === 'true' ||
      objects.length > 2 || uploads.length > 2 ||
      objects.some(object => !object.name.startsWith(prefix)) ||
      uploads.some(upload => !upload.name.startsWith(prefix)))
    fail('Unexpected data under the disposable account prefix');
  return { objects,uploads };
}

async function cleanup(storage,account) {
  const { objects,uploads } = await residual(storage,account);
  for (const upload of uploads) {
    if (typeof upload.uploadId !== 'string' || !upload.uploadId)
      fail('OSS returned an invalid upload ID during cleanup');
    await storage.abortMultipartUpload(upload.name,upload.uploadId);
  }
  for (const object of objects) await storage.delete(object.name);
  const after = await residual(storage,account);
  if (after.objects.length || after.uploads.length)
    fail('Disposable account still has OSS data after cleanup');
}

async function sha256File(filename) {
  const hash = crypto.createHash('sha256');
  for await (const chunk of fsSync.createReadStream(filename)) hash.update(chunk);
  return hash.digest('hex');
}

async function sha256SignedRead(readURL, internal = false) {
  const url = new URL(readURL);
  if (url.protocol !== 'https:' ||
      url.hostname !== ossHostname(internal))
    fail('Account service returned a read URL outside the dedicated OSS bucket');
  const response = await fetch(url,{ redirect:'error',
    signal:AbortSignal.timeout(30*60_000) });
  if (response.status !== 200 || Number(response.headers.get('content-length')) !== BYTES)
    fail(`Signed OSS read has unexpected status or size: HTTP ${response.status}`);
  const hash = crypto.createHash('sha256');
  let count = 0;
  for await (const chunk of response.body) { hash.update(chunk); count += chunk.length; }
  if (count !== BYTES) fail('Signed OSS read ended before 2 GB');
  return hash.digest('hex');
}

async function execute(values) {
  auditStage = 'credentials';
  if (process.getuid?.() !== 0) fail('Run acceptance on ECS as root with root-only RAM file');
  await ensureRecoveryDir(RECOVERY_DIR);
  const binary = await fs.lstat(values['client-binary']);
  if (!binary.isFile() || !path.isAbsolute(values['client-binary']))
    fail('C++ client binary must be an absolute regular file');
  const credentials = await readServerCredentials(values['env-file']);
  const OSS = require('../../oss-media-signer/node_modules/ali-oss');
  const storage = new OSS({
    accessKeyId:credentials.MAICHAT_CLOUD_DRIVE_OSS_ACCESS_KEY_ID,
    accessKeySecret:credentials.MAICHAT_CLOUD_DRIVE_OSS_ACCESS_KEY_SECRET,
    bucket:BUCKET,region:REGION,authorizationV4:true,secure:true,
    ...(values.ossInternal ? { internal:true } : {}) });
  auditStage = 'bucket_preflight';
  await preflight(storage);
  auditStage = 'private_workdir';
  // The ECS /tmp is a 1.8 GB tmpfs. Keep the sparse 2 GB source and SQLite
  // on its root disk, inside this restricted acceptance-only work directory.
  await fs.mkdir(WORK_ROOT,{ recursive:true,mode:0o700 });
  const workRoot = await fs.lstat(WORK_ROOT);
  if (!workRoot.isDirectory() || workRoot.isSymbolicLink() ||
      workRoot.uid !== 0 || (workRoot.mode & 0o077) !== 0)
    fail('Acceptance work directory is not root-only');
  const previousUmask = process.umask(0o077);
  const directory = await fs.mkdtemp(path.join(WORK_ROOT,'run-'));
  process.umask(previousUmask);
  const account = `cloudtest_${crypto.randomBytes(6).toString('hex')}`;
  const peer = `cloudpeer_${crypto.randomBytes(6).toString('hex')}`;
  const journal = path.join(RECOVERY_DIR,`client-${crypto.randomUUID()}.json`);
  const record = { bucket:BUCKET,region:REGION,account_id:account,
    work_dir:directory,created_at:new Date().toISOString(),stage:'before_upload' };
  await writeRecovery(journal,record,true);
  let database, signerServer, authServer, tlsServer, primaryError, cleanupError;
  try {
    auditStage = 'source_file';
    const initial = await residual(storage,account);
    if (initial.objects.length || initial.uploads.length)
      fail('Disposable account prefix was unexpectedly in use');
    const source = path.join(directory,'acceptance-2gb.mp4');
    const checkpoint = path.join(directory,'acceptance.maicloud.json');
    // Sparse zero bytes exercise the real 2 GB transport boundary. Playback
    // decoding still needs a separate valid small MP4 test in each app.
    const file = await fs.open(source,'wx',0o600);
    try { await file.truncate(BYTES); await file.sync(); } finally { await file.close(); }
    const sourceHash = await sha256File(source);
    auditStage = 'loopback_certificate';
    const certificate = await makeCertificate(directory,values['system-ca']);
    const internalKey = crypto.randomBytes(32).toString('hex');
    const signerHandler = createSignerHandler({ env:{ ...credentials,
      MAICHAT_CLOUD_DRIVE_ENABLED:'1',
      MAICHAT_CLOUD_DRIVE_OSS_BUCKET:BUCKET,
      MAICHAT_CLOUD_DRIVE_OSS_REGION:REGION,
      MAICHAT_CLOUD_DRIVE_INTERNAL_KEY:internalKey },
    ...(values.ossInternal ? { makeClient:configuration => new OSS({
      ...configuration,internal:true }) } : {}) });
    signerServer = createSignerServer(signerHandler);
    const signerPort = await listen(signerServer);
    auditStage = 'temporary_auth_store';
    database = openStore(path.join(directory,'accounts.sqlite'));
    const service = createAuthService({ db:database,sms:{ async send() {} },
      secret:crypto.randomBytes(32).toString('hex'),
      imSecret:crypto.randomBytes(32).toString('hex'),sdkAppId:1,
      cloudDrive:cloudDriveFromEnv({ MAICHAT_CLOUD_DRIVE_ENABLED:'1',
        MAICHAT_CLOUD_DRIVE_INTERNAL_KEY:internalKey,
        MAICHAT_MEDIA_PORT:String(signerPort) }) });
    authServer = createAuthServer(service);
    const authPort = await listen(authServer);
    tlsServer = gateway(authPort,certificate);
    const gatewayPort = await listen(tlsServer);
    auditStage = 'temporary_accounts';
    const base = `http://127.0.0.1:${authPort}`;
    const password = crypto.randomBytes(24).toString('base64url');
    for (const id of [account,peer]) {
      const result = await service.createTestAccount(id,password);
      if (result.statusCode !== 201) fail('Could not create disposable account');
    }
    const token = await login(base,account,password);
    const peerToken = await login(base,peer,password);
    auditStage = 'first_part_upload';
    const settings = { authBase:`https://localhost:${gatewayPort}`,
      token,ca:certificate.bundle,source,checkpoint };
    await runClient(values['client-binary'],'pause',settings);
    const pending = JSON.parse(await fs.readFile(checkpoint,'utf8'));
    if (!ID_PATTERN.test(pending.video_id) || typeof pending.upload_id !== 'string')
      fail('C++ checkpoint did not preserve the real OSS upload');
    const row = database.prepare('SELECT object_key,upload_id,status FROM cloud_drive_videos WHERE id=?')
      .get(pending.video_id);
    if (!row || row.upload_id !== pending.upload_id || row.status !== 'uploading' ||
        !row.object_key.startsWith(testPrefix(account)))
      fail('Disposable DB did not retain the resumable session');
    record.stage = 'paused';
    record.video_id = pending.video_id;
    record.object_key = row.object_key;
    record.upload_id = row.upload_id;
    await writeRecovery(journal,record);
    const partReply = await call(base,token,'GET',
      `/v1/cloud-drive/videos/uploads/${pending.video_id}/parts`);
    if (partReply.status !== 200 || !Array.isArray(partReply.body.parts) ||
        partReply.body.parts.length < 1 ||
        partReply.body.parts[0].size_bytes !== PART_BYTES)
      fail('OSS did not retain the first part for restart');
    console.log('Real OSS pause: first 8 MiB part confirmed by account ListParts');
    auditStage = 'resumed_upload';
    await runClient(values['client-binary'],'resume',settings);
    if (fsSync.existsSync(checkpoint)) fail('Completed upload left a C++ checkpoint');
    const completed = database.prepare('SELECT id,object_key,status FROM cloud_drive_videos WHERE id=?')
      .get(pending.video_id);
    if (completed?.status !== 'ready' || completed.object_key !== row.object_key)
      fail('Resume changed the upload session or did not complete');
    const head = await storage.head(row.object_key);
    const objectHeaders = head.res?.headers || head.headers || {};
    if (Number(objectHeaders['content-length']) !== BYTES ||
        objectHeaders['content-type'] !== 'video/mp4')
      fail('Completed OSS object has an unexpected size or type');
    const ownerList = await call(base,token,'GET','/v1/cloud-drive/videos');
    const peerList = await call(base,peerToken,'GET','/v1/cloud-drive/videos');
    if (ownerList.status !== 200 || ownerList.body.videos?.length !== 1 ||
        ownerList.body.videos[0].id !== pending.video_id ||
        ownerList.body.quota?.used_bytes !== BYTES ||
        peerList.status !== 200 || peerList.body.videos?.length !== 0)
      fail('List or account quota/isolation failed after resume');
    if ((await call(base,peerToken,'GET',
      `/v1/cloud-drive/videos/${pending.video_id}/play-link`)).status !== 404)
      fail('Other account obtained a private play link');
    const play = await call(base,token,'GET',
      `/v1/cloud-drive/videos/${pending.video_id}/play-link`);
    if (play.status !== 200 || play.body.read_expires_in !== 60 ||
        typeof play.body.read_url !== 'string')
      fail('Account service did not issue a short private play link');
    const readHash = await sha256SignedRead(play.body.read_url,
      values.ossInternal === true);
    auditStage = 'delete_cleanup';
    if (readHash !== sourceHash) fail('2 GB signed OSS read differs from C++ source');
    console.log('Real OSS resume/list/read: 2,000,000,000 bytes and SHA-256 match');
    const deleted = await call(base,token,'DELETE',
      `/v1/cloud-drive/videos/${pending.video_id}`);
    if (deleted.status !== 200 && deleted.status !== 202)
      fail('Account service could not delete the test video');
    if ((await call(base,token,'GET','/v1/cloud-drive/videos')).body.videos?.length)
      fail('Deleted video remained in the account list');
    await cleanup(storage,account);
    console.log('Real OSS cleanup: disposable account objects and uploads = 0');
  } catch (error) { primaryError = error; }
  finally {
    try { await cleanup(storage,account); }
    catch (error) { cleanupError = error; }
    await close(tlsServer);
    await close(authServer);
    await close(signerServer);
    database?.close();
    if (!cleanupError) {
      await clearRecovery(journal);
      await fs.rm(directory,{ recursive:true,force:true });
    }
  }
  if (cleanupError) fail('Cleanup unproven; root-only recovery journal retained');
  if (primaryError) throw primaryError;
  console.log('Shared C++ client and disposable account service against real OSS: PASS');
}

async function main(args = process.argv.slice(2)) {
  const values = parseArgs(args);
  if (!values) {
    console.log('Use --env-file=/root/... --client-binary=/absolute/path --system-ca=/absolute/path --execute=' + BUCKET + ' [--oss-internal]');
    return;
  }
  await execute(values);
}

if (require.main === module) {
  main().catch(error => {
    // Never print underlying SDK errors: they can include signed URLs or keys.
    const message = error instanceof Error && /^(?:C\+\+ (?:cloud client|checkpoint)|Cleanup unproven|Real OSS|Disposable|Dedicated|Cloud|Account|OSS|Completed|List|Other|Deleted|Could not|Unexpected|Invalid|Signed|2 GB|Explicit)/.test(error.message)
      ? error.message : 'acceptance failed; inspect root-only recovery journal';
    const code = typeof error?.code === 'string' && /^[A-Za-z0-9_-]{1,64}$/.test(error.code)
      ? error.code : error?.name || 'Error';
    process.stderr.write(`Real client acceptance: FAIL stage=${auditStage} code=${code} (${message})\n`);
    process.exitCode = 1;
  });
}

module.exports = { parseArgs,ossHostname,preflight,residual,cleanup };
