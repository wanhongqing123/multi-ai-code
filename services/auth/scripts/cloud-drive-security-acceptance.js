// Small real-OSS security acceptance for the dedicated private cloud-drive bucket.
// Run only against disposable accounts in a temporary SQLite database on the ECS.
'use strict';

const crypto = require('node:crypto');
const fs = require('node:fs/promises');
const http = require('node:http');
const path = require('node:path');
const { setTimeout: delay } = require('node:timers/promises');
const { openStore } = require('../store');
const { createAuthService } = require('../service');
const { createServer: createAuthServer, cloudDriveFromEnv } = require('../server');
const { completeAccountDeletion } = require('../account-deletion');
const { createHandler: createSignerHandler } = require('../../oss-media-signer/index');
const { createServer: createSignerServer } = require('../../oss-media-signer/server');
const { readServerCredentials, ensureRecoveryDir, writeRecovery,
  clearRecovery, validateSignedPart, validateSignedRead, putSignedPart,
  getSignedObject, assertPrivateBucketPolicy } =
  require('../../oss-media-signer/scripts/cloud-drive-oss-smoke');

const BUCKET = 'maichat-private-cloud-drive-gz-20261025-f346b709';
const REGION = 'oss-cn-guangzhou';
const CREDENTIAL_FILE = '/etc/maichat/cloud-drive.env';
const RECOVERY_DIR = '/var/lib/maichat/oss-smoke-recovery';
const WORK_ROOT = '/var/lib/maichat/cloud-real-acceptance/work';
const PART_BYTES = 8 * 1024 * 1024;
const COMPLETE_HOLD_MS = 24 * 60 * 60 * 1000;
const ACCOUNT_PATTERN = /^test_[a-f0-9]{16}$/;
const JOURNAL_PATTERN = /^security-[a-f0-9-]{36}\.json$/;

function fail(message) { throw new Error(message); }

function parseArgs(args) {
  if (args.length === 1 && args[0] === '--help') return null;
  const values = {};
  for (const arg of args) {
    const match = /^--(execute|env-file|recover)=(.+)$/.exec(arg);
    if (!match || values[match[1]]) fail('Invalid security acceptance arguments');
    values[match[1]] = match[2];
  }
  if (values.execute !== BUCKET || values['env-file'] !== CREDENTIAL_FILE ||
      (values.recover && !JOURNAL_PATTERN.test(values.recover)) ||
      Object.keys(values).some(key => !['execute','env-file','recover'].includes(key)))
    fail('Explicit dedicated bucket and root-only cloud-drive credential file required');
  return values;
}

function safeCode(error) {
  return typeof error?.code === 'string' && /^[A-Za-z0-9_-]{1,64}$/.test(error.code)
    ? error.code : 'unknown';
}

async function requireRootOnlyCredentials(filename) {
  const stat = await fs.lstat(filename);
  if (!stat.isFile() || stat.isSymbolicLink() || stat.uid !== 0 ||
      (stat.mode & 0o777) !== 0o600)
    fail('Credential file must be root-owned mode 0600');
}

async function listen(server) {
  await new Promise((resolve,reject) => {
    server.once('error',reject);
    server.listen(0,'127.0.0.1',resolve);
  });
  return server.address().port;
}

async function close(server) {
  if (!server?.listening) return;
  server.closeAllConnections?.();
  await new Promise(resolve => server.close(resolve));
}

async function readJournal(filename) {
  const stat = await fs.lstat(filename);
  if (!stat.isFile() || stat.isSymbolicLink() || stat.uid !== 0 ||
      (stat.mode & 0o077) !== 0 || stat.size > 2048)
    fail('Recovery journal is not a small root-only regular file');
  let record;
  try { record = JSON.parse(await fs.readFile(filename,'utf8')); }
  catch { fail('Recovery journal is invalid'); }
  if (record?.bucket !== BUCKET || record.region !== REGION ||
      !ACCOUNT_PATTERN.test(record.account_id || '') ||
      typeof record.work_dir !== 'string' ||
      path.dirname(record.work_dir) !== WORK_ROOT ||
      !/^security-[A-Za-z0-9]{6}$/.test(path.basename(record.work_dir)) ||
      (record.complete_hold_until_ms !== null &&
       (!Number.isSafeInteger(record.complete_hold_until_ms) ||
        record.complete_hold_until_ms < 1)))
    fail('Recovery journal is outside disposable cloud-drive scope');
  return record;
}

function accountPrefix(account) {
  if (!ACCOUNT_PATTERN.test(account || '')) fail('Invalid disposable account');
  return `cloud-drive/${account}/`;
}

async function inventory(storage,account) {
  const prefix = accountPrefix(account);
  const [objectsPage,uploadsPage] = await Promise.all([
    storage.list({ prefix,'max-keys':10 }),
    storage.listUploads({ prefix,'max-uploads':10 })
  ]);
  const objects = objectsPage?.objects || [];
  const uploads = uploadsPage?.uploads || [];
  const validName = name => typeof name === 'string' &&
    name.startsWith(prefix) &&
    /^cloud-drive\/test_[a-f0-9]{16}\/\d{4}-\d{2}-\d{2}\/[a-f0-9-]{36}\.mp4$/.test(name);
  if ([true,'true'].includes(objectsPage?.isTruncated) ||
      [true,'true'].includes(uploadsPage?.isTruncated) ||
      !Array.isArray(objects) || !Array.isArray(uploads) ||
      objects.length > 2 || uploads.length > 2 ||
      objects.some(item => !validName(item?.name)) ||
      uploads.some(item => !validName(item?.name) ||
        typeof item.uploadId !== 'string' || !item.uploadId))
    fail('Unexpected data under disposable account prefix; manual recovery required');
  return { objects,uploads };
}

async function cleanup(storage,account) {
  const found = await inventory(storage,account);
  for (const upload of found.uploads)
    await storage.abortMultipartUpload(upload.name,upload.uploadId);
  for (const object of found.objects) await storage.delete(object.name);
  const after = await inventory(storage,account);
  if (after.objects.length || after.uploads.length)
    fail('Disposable account OSS data remains after cleanup');
}

async function recoverDisposable(storage,record,finalize,clock = Date.now) {
  await cleanup(storage,record.account_id);
  if (record.complete_hold_until_ms && clock() < record.complete_hold_until_ms)
    return false;
  await finalize();
  return true;
}

async function completeWithRecovery(prepare,settled,operation,
  knownFailure = () => false) {
  await prepare();
  let operationReturned = false;
  try {
    const value = await operation();
    operationReturned = true;
    await settled();
    return value;
  } catch (error) {
    if (!operationReturned && knownFailure(error)) await settled();
    throw error;
  }
}

function storageFor(credentials) {
  const OSS = require('../../oss-media-signer/node_modules/ali-oss');
  return new OSS({ accessKeyId:credentials.MAICHAT_CLOUD_DRIVE_OSS_ACCESS_KEY_ID,
    accessKeySecret:credentials.MAICHAT_CLOUD_DRIVE_OSS_ACCESS_KEY_SECRET,
    bucket:BUCKET,region:REGION,authorizationV4:true,secure:true });
}

async function authCall(base,token,method,route,body) {
  const response = await fetch(`${base}${route}`,{ method,redirect:'error',
    headers:{ Authorization:`Bearer ${token}`,'Content-Type':'application/json' },
    ...(body ? { body:JSON.stringify(body) } : {}) });
  let json;
  try { json = await response.json(); }
  catch { fail('Temporary account service returned invalid JSON'); }
  return { status:response.status,body:json };
}

async function signAndPut(base,token,videoId,uploadId,key,payload) {
  const md5 = crypto.createHash('md5').update(payload).digest('base64');
  const signed = await authCall(base,token,'POST',
    `/v1/cloud-drive/videos/uploads/${videoId}/parts/1/sign`,
    { content_md5_base64:md5 });
  if (signed.status !== 200 || signed.body.part_size_bytes !== PART_BYTES)
    fail('Temporary account service did not sign the expected part');
  const url = validateSignedPart(signed.body,BUCKET,REGION,key,uploadId,
    PART_BYTES,md5);
  const result = await putSignedPart(url,signed.body.upload_headers,payload);
  if (result.status !== 200) fail(`Real OSS part PUT failed: HTTP ${result.status || 0}`);
  const parts = await authCall(base,token,'GET',
    `/v1/cloud-drive/videos/uploads/${videoId}/parts`);
  if (parts.status !== 200 || parts.body.parts?.length !== 1 ||
      parts.body.parts[0].part_number !== 1 ||
      parts.body.parts[0].size_bytes !== PART_BYTES)
    fail('Real OSS ListParts did not verify the uploaded part');
}

async function signerCall(handler,secret,action,account,key,body = {}) {
  const response = await handler({ rawPath:`/cloud-drive/${action}`,httpMethod:'POST',
    headers:{ 'X-MaiChat-Cloud-Drive-Key':secret },
    body:JSON.stringify({ account_id:account,object_key:key,...body }) });
  if (response.statusCode !== 200)
    fail(`Temporary signer ${action} returned HTTP ${response.statusCode}`);
  try { return JSON.parse(response.body); }
  catch { fail('Temporary signer returned invalid JSON'); }
}

async function assertNoOverwrite({ storage,handler,secret,account,key,payload,hash,
  beforeComplete = async () => {},afterKnownComplete = async () => {} }) {
  let secondUpload;
  try {
    try {
      const initiated = await storage.initMultipartUpload(key,{
        mime:'video/mp4',headers:{ 'x-oss-object-acl':'private',
          'x-oss-forbid-overwrite':'true' } });
      secondUpload = initiated.uploadId;
      if (typeof secondUpload !== 'string' || !secondUpload)
        fail('OSS returned an invalid second upload ID');
    } catch (error) {
      if (error?.status !== 409 && error?.status !== 412)
        fail(`Second Multipart init failed for an unrelated reason: ${safeCode(error)}`);
      return { stage:'init',code:safeCode(error) };
    }
    const changed = Buffer.from(payload);
    changed[0] ^= 1;
    const md5 = crypto.createHash('md5').update(changed).digest('base64');
    const signed = await signerCall(handler,secret,'sign-part',account,key,
      { upload_id:secondUpload,part_number:1,expected_bytes:PART_BYTES,
        content_md5_base64:md5 });
    const url = validateSignedPart(signed,BUCKET,REGION,key,secondUpload,
      PART_BYTES,md5);
    const put = await putSignedPart(url,signed.upload_headers,changed);
    if (put.status !== 200) fail(`Second OSS part PUT failed: HTTP ${put.status || 0}`);
    const parts = await signerCall(handler,secret,'list-parts',account,key,
      { upload_id:secondUpload });
    if (parts.parts?.length !== 1 || parts.parts[0].size_bytes !== PART_BYTES)
      fail('Second Multipart ListParts did not return exactly one part');
    let refusal;
    try {
      await completeWithRecovery(beforeComplete,afterKnownComplete,
        () => storage.completeMultipartUpload(key,secondUpload,
          [{ number:1,etag:parts.parts[0].etag }],
          { headers:{ 'x-oss-forbid-overwrite':'true' } }),
        error => error?.status === 409 || error?.status === 412);
    } catch (error) { refusal = error; }
    if (!refusal || (refusal.status !== 409 && refusal.status !== 412))
      fail('OSS did not reject completion over an existing private object');
    return { stage:'complete',code:safeCode(refusal) };
  } finally {
    if (secondUpload) {
      try { await storage.abortMultipartUpload(key,secondUpload); }
      catch (error) {
        if (error?.status !== 404 && error?.code !== 'NoSuchUpload') throw error;
      }
    }
    const read = await signerCall(handler,secret,'read',account,key);
    const url = validateSignedRead(read,BUCKET,REGION,key);
    const actual = await getSignedObject(url,PART_BYTES);
    if (actual.status !== 200 || actual.size_bytes !== PART_BYTES ||
        actual.sha256 !== hash)
      fail('Original private object changed during overwrite refusal');
  }
}

async function assertReadExpiry(handler,secret,account,key,hash) {
  const signed = await signerCall(handler,secret,'read',account,key);
  const url = validateSignedRead(signed,BUCKET,REGION,key);
  const immediate = await getSignedObject(url,PART_BYTES);
  if (immediate.status !== 200 || immediate.size_bytes !== PART_BYTES ||
      immediate.sha256 !== hash)
    fail('60-second signed GET was not immediately readable');
  await delay(75_000);
  const expired = await getSignedObject(url,PART_BYTES);
  if (expired.status !== 403) fail(`Expired signed GET returned HTTP ${expired.status || 0}`);
  return expired.status;
}

function deletionEvidence() {
  const records = {};
  const evidence = { operator:'cloud_acceptance',records };
  for (const key of ['tencent_im_deleted','session_sync_deleted',
    'oss_and_ark_assets_reviewed','shared_content_reviewed']) {
    evidence[key] = true;
    records[key] = 'disposable-local-test-account-no-external-assets';
  }
  return evidence;
}

async function run(values) {
  if (process.getuid?.() !== 0) fail('Run only as root on ECS');
  await ensureRecoveryDir(RECOVERY_DIR);
  await requireRootOnlyCredentials(values['env-file']);
  const credentials = await readServerCredentials(values['env-file']);
  const storage = storageFor(credentials);
  await assertPrivateBucketPolicy(storage,BUCKET);
  if (values.recover) {
    const journal = path.join(RECOVERY_DIR,values.recover);
    const record = await readJournal(journal);
    const finished = await recoverDisposable(storage,record,async () => {
      let workStat;
      try { workStat = await fs.lstat(record.work_dir); }
      catch (error) { if (error?.code !== 'ENOENT') throw error; }
      if (workStat && (!workStat.isDirectory() || workStat.isSymbolicLink() ||
          workStat.uid !== 0 || (workStat.mode & 0o077) !== 0))
        fail('Disposable acceptance work directory is unsafe to remove');
      if (workStat) await fs.rm(record.work_dir,{ recursive:true,force:true });
      await clearRecovery(journal);
    });
    if (!finished) fail('Recovery hold active after uncertain Complete; retry after 24 hours');
    console.log('Disposable cloud-drive prefix recovery: clean');
    return;
  }
  await fs.mkdir(WORK_ROOT,{ recursive:true,mode:0o700 });
  const workStat = await fs.lstat(WORK_ROOT);
  if (!workStat.isDirectory() || workStat.isSymbolicLink() ||
      workStat.uid !== 0 || (workStat.mode & 0o077) !== 0)
    fail('Acceptance work directory must be root-only');
  const account = `test_${crypto.randomBytes(8).toString('hex')}`;
  const initial = await inventory(storage,account);
  if (initial.objects.length || initial.uploads.length)
    fail('Random disposable account prefix was unexpectedly occupied');
  const work = await fs.mkdtemp(path.join(WORK_ROOT,'security-'));
  await fs.chmod(work,0o700);
  const journal = path.join(RECOVERY_DIR,`security-${crypto.randomUUID()}.json`);
  let record = { bucket:BUCKET,region:REGION,account_id:account,
    work_dir:work,
    created_at:new Date().toISOString(),complete_hold_until_ms:null };
  await writeRecovery(journal,record,true);
  const updateHold = async until => {
    const next = { ...record,complete_hold_until_ms:until };
    await writeRecovery(journal,next);
    record = next;
  };
  const beforeComplete = () => updateHold(Date.now()+COMPLETE_HOLD_MS);
  const afterKnownComplete = () => updateHold(null);
  let database,signerServer,authServer,primaryError,cleanupError;
  try {
    const secret = crypto.randomBytes(32).toString('hex');
    const signerHandler = createSignerHandler({ env:{ ...credentials,
      MAICHAT_CLOUD_DRIVE_ENABLED:'1',
      MAICHAT_CLOUD_DRIVE_OSS_BUCKET:BUCKET,
      MAICHAT_CLOUD_DRIVE_OSS_REGION:REGION,
      MAICHAT_CLOUD_DRIVE_INTERNAL_KEY:secret } });
    signerServer = createSignerServer(signerHandler);
    const signerPort = await listen(signerServer);
    database = openStore(path.join(work,'accounts.sqlite'));
    const authSecret = crypto.randomBytes(32).toString('hex');
    const service = createAuthService({ db:database,sms:{ async send() {} },
      secret:authSecret,imSecret:crypto.randomBytes(32).toString('hex'),
      sdkAppId:1,cloudDrive:cloudDriveFromEnv({
        MAICHAT_CLOUD_DRIVE_ENABLED:'1',
        MAICHAT_CLOUD_DRIVE_INTERNAL_KEY:secret,
        MAICHAT_MEDIA_PORT:String(signerPort) }) });
    authServer = createAuthServer(service);
    const authPort = await listen(authServer);
    const base = `http://127.0.0.1:${authPort}`;
    const password = crypto.randomBytes(24).toString('base64url');
    if ((await service.createTestAccount(account,password)).statusCode !== 201)
      fail('Could not create disposable account');
    const login = await fetch(`${base}/v1/auth/password/login`,{
      method:'POST',headers:{ 'Content-Type':'application/json' },
      body:JSON.stringify({ account_id:account,password,device_slot:'ios_phone',
        age_confirmed_14_plus:true }) });
    if (login.status !== 200) fail('Disposable account login failed');
    const token = (await login.json()).access_token;
    if (typeof token !== 'string') fail('Disposable login returned no access token');
    const payload = crypto.randomBytes(PART_BYTES);
    const hash = crypto.createHash('sha256').update(payload).digest('hex');
    const created = await authCall(base,token,'POST','/v1/cloud-drive/videos/uploads',
      { name:'security-ready.mp4',size_bytes:PART_BYTES,content_type:'video/mp4' });
    if (created.status !== 201) fail('Could not create disposable ready video');
    const readyId = created.body.video_id;
    const ready = database.prepare('SELECT object_key,upload_id FROM cloud_drive_videos WHERE id=?')
      .get(readyId);
    if (!ready?.object_key.startsWith(accountPrefix(account)) || !ready.upload_id)
      fail('Temporary SQLite stored an unexpected object');
    await signAndPut(base,token,readyId,ready.upload_id,ready.object_key,payload);
    await completeWithRecovery(beforeComplete,afterKnownComplete,async () => {
      if ((await authCall(base,token,'POST',
        `/v1/cloud-drive/videos/uploads/${readyId}/complete`)).status !== 200)
        fail('Could not complete disposable video');
    });
    const first = await getSignedObject(validateSignedRead(
      await signerCall(signerHandler,secret,'read',account,ready.object_key),
      BUCKET,REGION,ready.object_key),PART_BYTES);
    if (first.status !== 200 || first.sha256 !== hash)
      fail('Completed disposable video differs from source bytes');
    const overwrite = await assertNoOverwrite({ storage,handler:signerHandler,
      secret,account,key:ready.object_key,payload,hash,
      beforeComplete,afterKnownComplete });
    console.log(`OSS forbid-overwrite: rejected at ${overwrite.stage}, code=${overwrite.code}; original SHA-256 unchanged`);
    await assertReadExpiry(signerHandler,secret,account,ready.object_key,hash);
    console.log('OSS signed GET: immediate HTTP 200, same URL after 75 seconds HTTP 403');
    const pending = await authCall(base,token,'POST','/v1/cloud-drive/videos/uploads',
      { name:'security-pending.mp4',size_bytes:2*PART_BYTES,content_type:'video/mp4' });
    if (pending.status !== 201) fail('Could not create disposable pending video');
    const partial = database.prepare('SELECT object_key,upload_id FROM cloud_drive_videos WHERE id=?')
      .get(pending.body.video_id);
    if (!partial?.object_key.startsWith(accountPrefix(account)) || !partial.upload_id)
      fail('Temporary SQLite stored an unexpected pending object');
    await signAndPut(base,token,pending.body.video_id,partial.upload_id,
      partial.object_key,payload);
    const deletion = await authCall(base,token,'POST','/v1/auth/account/deletion',
      { confirm_account_id:account,password });
    if (deletion.status !== 202 || deletion.body.cloud_video_cleanup_pending !== false)
      fail('Disposable account deletion did not finish cloud cleanup');
    const uncleared = database.prepare(`SELECT COUNT(*) AS count FROM cloud_drive_videos
      WHERE owner_user_id=(SELECT id FROM users WHERE account_id=?) AND
      (status<>'deleted' OR cleanup_pending<>0)`).get(account).count;
    if (uncleared) fail('Disposable cloud metadata still needs OSS cleanup');
    const empty = await inventory(storage,account);
    if (empty.objects.length || empty.uploads.length)
      fail('Disposable account deletion left an OSS object or Multipart upload');
    const finalized = completeAccountDeletion(database,deletion.body.request_id,
      deletionEvidence(),authSecret);
    if (finalized.status !== 'completed' ||
        database.prepare('SELECT id FROM users WHERE account_id=?').get(account) ||
        database.pragma('foreign_key_check').length)
      fail('Disposable account deletion did not finalize cleanly');
    console.log('Disposable account deletion: ready object + partial upload cleaned; SQLite finalized');
  } catch (error) { primaryError = error; }
  finally {
    try { await cleanup(storage,account); }
    catch (error) { cleanupError = error; }
    await close(authServer);
    await close(signerServer);
    database?.close();
    if (!cleanupError && (!record.complete_hold_until_ms ||
        Date.now() >= record.complete_hold_until_ms)) {
      await fs.rm(work,{ recursive:true,force:true });
      await clearRecovery(journal);
    }
  }
  if (cleanupError) fail('Cleanup unproven; root-only recovery journal retained');
  if (record.complete_hold_until_ms && Date.now() < record.complete_hold_until_ms)
    fail('Recovery hold active after uncertain Complete; root-only journal retained');
  if (primaryError) throw primaryError;
  console.log('Real OSS cloud-drive security acceptance: PASS');
}

async function main(args = process.argv.slice(2)) {
  const values = parseArgs(args);
  if (!values) {
    console.log(`Use --env-file=${CREDENTIAL_FILE} --execute=${BUCKET} [--recover=security-UUID.json]`);
    return;
  }
  await run(values);
}

if (require.main === module) {
  main().catch(error => {
    const message = typeof error?.message === 'string' &&
      /^(?:Run only|Explicit|Recovery|Unexpected|Disposable|Could not|Real OSS|OSS|Temporary|Original|Second|60-second|Expired|Account|Random|Completed|Cleanup|Acceptance)/.test(error.message)
      ? error.message : 'acceptance failed; inspect root-only recovery journal';
    process.stderr.write(`Cloud-drive security acceptance: FAIL (${message})\n`);
    process.exitCode = 1;
  });
}

module.exports = { parseArgs,accountPrefix,inventory,cleanup,recoverDisposable,
  completeWithRecovery,deletionEvidence,assertNoOverwrite,assertReadExpiry };
