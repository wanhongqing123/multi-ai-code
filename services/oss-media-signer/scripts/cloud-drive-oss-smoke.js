// Real OSS smoke probe for a newly created, empty, dedicated cloud-drive bucket.
// Run only on the ECS with the existing media service credential file.
'use strict';

const crypto = require('node:crypto');
const fs = require('node:fs/promises');
const https = require('node:https');
const path = require('node:path');
const { createHandler } = require('../index');

const PART_BYTES = 8 * 1024 * 1024;
const RECOVERY_DIR = '/var/lib/maichat/oss-smoke-recovery';
const FORBIDDEN_BUCKETS = new Set(['maichat-ai-video-resource']);

class ProbeError extends Error {}

function safeErrorCode(error) {
  const code = typeof error?.code === 'string' && /^[A-Za-z0-9_-]{1,64}$/.test(error.code)
    ? error.code : 'request_failed';
  const status = Number.isInteger(error?.status) && error.status >= 100 && error.status <= 599
    ? error.status : 0;
  return `${code}${status ? ` HTTP ${status}` : ''}`;
}

function parseArgs(args) {
  const values = {};
  for (const arg of args) {
    if (arg === '--positive-only') {
      if (values.positiveOnly) throw new ProbeError('Duplicate smoke mode');
      values.positiveOnly = true;
      continue;
    }
    if (arg === '--complete-roundtrip') {
      if (values.completeRoundtrip) throw new ProbeError('Duplicate smoke mode');
      values.completeRoundtrip = true;
      continue;
    }
    if (arg.startsWith('--recover=')) {
      if (values.recover) throw new ProbeError('Duplicate recovery journal');
      values.recover = arg.slice('--recover='.length);
      continue;
    }
    const match = /^--(bucket|region|env-file|confirm-new-empty-bucket)=(.+)$/.exec(arg);
    if (!match || values[match[1]]) throw new ProbeError('Invalid smoke arguments');
    values[match[1]] = match[2];
  }
  if (!values.bucket || !values.region || !values['env-file'] ||
      !values['confirm-new-empty-bucket'] ||
      (values.positiveOnly && values.recover) ||
      (values.completeRoundtrip && (values.positiveOnly || values.recover)) ||
      (values.recover && !/^[a-f0-9]{8}-[a-f0-9]{4}-4[a-f0-9]{3}-[89ab][a-f0-9]{3}-[a-f0-9]{12}\.json$/.test(values.recover)))
    throw new ProbeError('Missing or invalid smoke arguments');
  assertTarget(values.bucket, values.region, values['confirm-new-empty-bucket']);
  return { bucket: values.bucket, region: values.region, envFile: values['env-file'],
    positiveOnly: !!values.positiveOnly, completeRoundtrip: !!values.completeRoundtrip,
    recover: values.recover || '' };
}

function assertTarget(bucket, region, confirmedBucket) {
  if (!/^[a-z0-9][a-z0-9-]{1,61}[a-z0-9]$/.test(bucket || '') ||
      !/^oss-[a-z0-9-]+$/.test(region || '') ||
      FORBIDDEN_BUCKETS.has(bucket) || bucket !== confirmedBucket)
    throw new ProbeError('Target must be an explicitly confirmed new cloud-drive bucket');
}

async function readServerCredentials(filename) {
  const file = await fs.lstat(filename);
  if (!file.isFile() || file.isSymbolicLink() || file.uid !== 0 || (file.mode & 0o007) !== 0)
    throw new ProbeError('Credential file must be root-owned and inaccessible to others');
  const content = await fs.readFile(filename, 'utf8');
  const wanted = new Set(['MAICHAT_CLOUD_DRIVE_OSS_ACCESS_KEY_ID',
    'MAICHAT_CLOUD_DRIVE_OSS_ACCESS_KEY_SECRET']);
  const found = {};
  for (const line of content.split(/\r?\n/)) {
    const trimmed = line.trim();
    if (!trimmed || trimmed.startsWith('#')) continue;
    const match = /^(?:export\s+)?([A-Z][A-Z0-9_]*)=(.*)$/.exec(trimmed);
    if (!match || !wanted.has(match[1])) continue;
    let value = match[2].trim();
    if ((value.startsWith('"') && value.endsWith('"')) ||
        (value.startsWith("'") && value.endsWith("'"))) value = value.slice(1, -1);
    if (!value || found[match[1]]) throw new ProbeError('Credential file has a missing or duplicate OSS key');
    found[match[1]] = value;
  }
  if (Object.keys(found).length !== wanted.size)
    throw new ProbeError('Credential file is missing OSS keys');
  return found;
}

async function ensureRecoveryDir(directory = RECOVERY_DIR) {
  if (process.getuid?.() !== 0) throw new ProbeError('Run the real OSS smoke as root');
  await fs.mkdir(directory, { recursive: true, mode: 0o700 });
  const stat = await fs.lstat(directory);
  if (!stat.isDirectory() || stat.isSymbolicLink() || stat.uid !== 0 ||
      (stat.mode & 0o077) !== 0)
    throw new ProbeError('Recovery directory must be root-only');
}

async function writeRecovery(filename, record, create = false) {
  const output = `${JSON.stringify(record)}\n`;
  const target = create ? filename : `${filename}.${crypto.randomUUID()}.tmp`;
  const file = await fs.open(target, 'wx', 0o600);
  try { await file.writeFile(output); await file.sync(); }
  finally { await file.close(); }
  if (!create) await fs.rename(target, filename);
  const directory = await fs.open(path.dirname(filename), 'r');
  try { await directory.sync(); } finally { await directory.close(); }
}

async function clearRecovery(filename) {
  await fs.unlink(filename);
  const directory = await fs.open(path.dirname(filename), 'r');
  try { await directory.sync(); } finally { await directory.close(); }
}

async function readRecovery(filename, bucket, region) {
  const stat = await fs.lstat(filename);
  if (!stat.isFile() || stat.isSymbolicLink() || stat.uid !== 0 ||
      (stat.mode & 0o077) !== 0 || stat.size > 4096)
    throw new ProbeError('Recovery journal must be a small root-only regular file');
  let record;
  try { record = JSON.parse(await fs.readFile(filename, 'utf8')); }
  catch { throw new ProbeError('Recovery journal is not valid JSON'); }
  validateRecoveryRecord(record, bucket, region);
  return record;
}

function validateRecoveryRecord(record, bucket, region) {
  const accountId = record?.account_id;
  const key = record?.object_key;
  const uploadId = record?.upload_id;
  const phase = record?.phase;
  if (record?.bucket !== bucket || record?.region !== region ||
      !/^test_[a-f0-9]{16}$/.test(accountId || '') ||
      !new RegExp(`^cloud-drive/${accountId}/\\d{4}-\\d{2}-\\d{2}/` +
        '[a-f0-9]{8}-[a-f0-9]{4}-4[a-f0-9]{3}-[89ab][a-f0-9]{3}-[a-f0-9]{12}\\.mp4$')
        .test(key || '') ||
      (uploadId !== null &&
        (typeof uploadId !== 'string' || !/^[A-Za-z0-9_+\/=\-]{1,512}$/.test(uploadId))) ||
      (phase !== undefined && !['init_pending', 'uploading', 'completed',
        'deleting', 'deleted'].includes(phase)))
    throw new ProbeError('Recovery journal is outside the new test account and bucket');
}

function recoveryUploadIds(record, uploads) {
  if (uploads?.isTruncated || !Array.isArray(uploads?.uploads) ||
      uploads.uploads.some(item => item.name !== record.object_key ||
        typeof item.uploadId !== 'string' ||
        !/^[A-Za-z0-9_+\/=\-]{1,512}$/.test(item.uploadId)))
    throw new ProbeError('Recovery inventory is outside the exact test object');
  return new Set([record.upload_id, ...uploads.uploads.map(item => item.uploadId)]
    .filter(Boolean));
}

async function assertPrivateBucketPolicy(storage, bucket) {
  const [acl, versioning, lifecycle] = await Promise.all([
    storage.getBucketACL(bucket),
    storage.getBucketVersioning(bucket),
    storage.getBucketLifecycle(bucket)
  ]);
  if (acl.acl !== 'private') throw new ProbeError('Cloud-drive bucket must be private');
  // OSS omits Status when versioning has never been enabled.
  if (versioning.versionStatus !== undefined && versioning.versionStatus !== 'Disabled')
    throw new ProbeError('Cloud-drive bucket versioning must have never been enabled');
  const rules = lifecycle.rules || [];
  const applies = rules.filter(rule => rule.status === 'Enabled' &&
    typeof rule.prefix === 'string' && 'cloud-drive/'.startsWith(rule.prefix));
  if (!applies.some(rule => rule.prefix === 'cloud-drive/' &&
      Number(rule.abortMultipartUpload?.days) === 8) ||
      applies.some(rule => rule.abortMultipartUpload &&
        (!Number.isFinite(Number(rule.abortMultipartUpload.days)) ||
         Number(rule.abortMultipartUpload.days) < 8)))
    throw new ProbeError('Cloud-drive bucket needs an 8-day multipart abort rule');
}

async function assertEmptyPrivateBucket(storage, bucket) {
  await assertPrivateBucketPolicy(storage, bucket);
  // The dedicated RAM identity may list only cloud-drive/*; whole-bucket emptiness
  // is checked separately with the admin identity when the new bucket is created.
  const [objects, uploads] = await Promise.all([
    storage.list({ prefix: 'cloud-drive/', 'max-keys': 1 }),
    storage.listUploads({ prefix: 'cloud-drive/', 'max-uploads': 1 })
  ]);
  if ((objects.objects || []).length || (objects.prefixes || []).length ||
      objects.isTruncated || (uploads.uploads || []).length || uploads.isTruncated)
    throw new ProbeError('Cloud-drive prefix is not empty; smoke refused');
}

function validateSignedPart(signed, bucket, region, key, uploadId, length, md5) {
  if (signed.upload_headers?.['Content-Length'] !== String(length) ||
      signed.upload_headers?.['Content-MD5'] !== md5)
    throw new ProbeError('Signer returned different part headers');
  let url;
  try { url = new URL(signed.upload_url); }
  catch { throw new ProbeError('Signer returned an invalid URL'); }
  if (url.protocol !== 'https:' ||
      url.hostname !== `${bucket}.${region}.aliyuncs.com` ||
      decodeURIComponent(url.pathname) !== `/${key}` ||
      url.searchParams.get('partNumber') !== '1' ||
      url.searchParams.get('uploadId') !== uploadId ||
      url.searchParams.get('x-oss-additional-headers') !== 'content-length')
    throw new ProbeError('Signer URL is not scoped to the test part and bucket');
  return url;
}

async function putSignedPart(url, headers, buffer, requestFactory = https.request) {
  return new Promise(resolve => {
    let phase = 'connect';
    let settled = false;
    const finish = outcome => {
      if (settled) return;
      settled = true;
      resolve(outcome);
    };
    const transportFailure = error => finish({ status: 0,
      transport_code: typeof error?.code === 'string' &&
        /^[A-Za-z0-9_-]{1,64}$/.test(error.code) ? error.code : 'TRANSPORT_ERROR',
      phase });
    const request = requestFactory(url, { method: 'PUT', headers, timeout: 120000 }, response => {
      phase = 'receive';
      let body = '';
      response.setEncoding('utf8');
      response.on('data', chunk => { if (body.length < 8192) body += chunk.slice(0, 8192 - body.length); });
      response.on('end', () => {
        const code = /<Code>([A-Za-z0-9_-]{1,64})<\/Code>/.exec(body)?.[1] || '';
        finish({ status: response.statusCode, code });
      });
      response.on('error', transportFailure);
      response.on('aborted', () => transportFailure({ code: 'RESPONSE_ABORTED' }));
    });
    request.on('socket', socket => {
      // OSS may close a deliberately invalid PUT while Node is still writing.
      // TLSSocket can emit EPIPE directly, without forwarding it to ClientRequest.
      socket.on('error', transportFailure);
      socket.on('secureConnect', () => {
        if (phase === 'connect') phase = 'send';
      });
    });
    request.on('finish', () => { if (phase !== 'receive') phase = 'await_response'; });
    request.on('timeout', () => request.destroy(Object.assign(new Error('timeout'),
      { code: 'ETIMEDOUT' })));
    request.on('error', transportFailure);
    try { request.end(buffer); }
    catch (error) { transportFailure(error); }
  });
}

function putOutcome(outcome) {
  if (Number.isInteger(outcome?.status) && outcome.status >= 100 && outcome.status <= 599)
    return `HTTP ${outcome.status}${
      /^[A-Za-z0-9_-]{1,64}$/.test(outcome.code || '') ? ` ${outcome.code}` : ''}`;
  if (outcome?.status === 0 && /^[A-Za-z0-9_-]{1,64}$/.test(outcome.transport_code || '') &&
      ['connect', 'send', 'await_response', 'receive'].includes(outcome.phase))
    return `transport ${outcome.transport_code} phase=${outcome.phase}`;
  throw new ProbeError('OSS PUT returned an invalid outcome');
}

function validateSignedRead(signed, bucket, region, key) {
  if (signed?.read_expires_in !== 60) throw new ProbeError('Signer returned the wrong GET TTL');
  let url;
  try { url = new URL(signed.read_url); }
  catch { throw new ProbeError('Signer returned an invalid GET URL'); }
  if (url.protocol !== 'https:' ||
      url.hostname !== `${bucket}.${region}.aliyuncs.com` ||
      decodeURIComponent(url.pathname) !== `/${key}`)
    throw new ProbeError('Signer GET URL is outside the test bucket and object');
  return url;
}

async function getSignedObject(url, expectedBytes, requestFactory = https.request) {
  return new Promise(resolve => {
    let phase = 'connect';
    let settled = false;
    const finish = outcome => {
      if (settled) return;
      settled = true;
      resolve(outcome);
    };
    const transportFailure = error => finish({ status: 0,
      transport_code: typeof error?.code === 'string' &&
        /^[A-Za-z0-9_-]{1,64}$/.test(error.code) ? error.code : 'TRANSPORT_ERROR',
      phase });
    const request = requestFactory(url, { method: 'GET', timeout: 120000 }, response => {
      phase = 'receive';
      const hash = crypto.createHash('sha256');
      let bytes = 0;
      response.on('data', chunk => {
        bytes += chunk.length;
        if (bytes > expectedBytes) {
          request.destroy(Object.assign(new Error('oversized GET'), { code: 'TOO_LARGE' }));
          return;
        }
        hash.update(chunk);
      });
      response.on('end', () => finish({ status: response.statusCode,
        size_bytes: bytes, sha256: hash.digest('hex') }));
      response.on('error', transportFailure);
      response.on('aborted', () => transportFailure({ code: 'RESPONSE_ABORTED' }));
    });
    request.on('socket', socket => {
      socket.on('error', transportFailure);
      socket.on('secureConnect', () => {
        if (phase === 'connect') phase = 'send';
      });
    });
    request.on('finish', () => { if (phase !== 'receive') phase = 'await_response'; });
    request.on('timeout', () => request.destroy(Object.assign(new Error('timeout'),
      { code: 'ETIMEDOUT' })));
    request.on('error', transportFailure);
    try { request.end(); }
    catch (error) { transportFailure(error); }
  });
}

async function callSigner(handler, secret, action, accountId, key, extra = {}) {
  const response = await handler({ rawPath: `/cloud-drive/${action}`, httpMethod: 'POST',
    headers: { 'X-MaiChat-Cloud-Drive-Key': secret },
    body: JSON.stringify({ account_id: accountId, object_key: key, ...extra }) });
  if (response.statusCode !== 200)
    throw new ProbeError(`Signer ${action} returned HTTP ${response.statusCode}`);
  try { return JSON.parse(response.body); }
  catch { throw new ProbeError(`Signer ${action} returned invalid JSON`); }
}

async function assertParts(storage, key, uploadId, expectedCount, expectedSize = 0,
                           log = () => {}) {
  let page;
  try { page = await storage.listParts(key, uploadId, { 'max-parts': 2 }); }
  catch (error) {
    log(`OSS ListParts: HTTP ${Number.isInteger(error?.status) ? error.status : 'unknown'} ` +
      `error=${safeErrorCode(error)}`);
    throw error;
  }
  const raw = page?.parts;
  const kind = Array.isArray(raw) ? 'array' :
    raw && typeof raw === 'object' ? 'object' :
      raw === undefined || raw === null ? 'empty' : 'invalid';
  const parts = kind === 'array' ? raw : kind === 'object' ? [raw] : [];
  const first = parts.length === 1 ? parts[0] : null;
  const keys = first && typeof first === 'object' ? Object.keys(first)
    .map(name => /^[A-Za-z][A-Za-z0-9_]{0,63}$/.test(name) ? name : '<invalid>') : [];
  const number = Number(first?.PartNumber ?? first?.partNumber);
  const size = Number(first?.Size ?? first?.size);
  const status = Number(page?.res?.status ?? page?.res?.statusCode ?? page?.status);
  log(`OSS ListParts: HTTP ${Number.isInteger(status) && status >= 100 && status <= 599 ?
    status : 'unknown'} ` +
    `parts=${kind} length=${parts.length} keys=${JSON.stringify(keys)} ` +
    `part_number=${first && Number.isSafeInteger(number) ? number : 'none'} ` +
    `size_bytes=${first && Number.isSafeInteger(size) ? size : 'none'}`);
  if (page?.isTruncated === true || page?.isTruncated === 'true' ||
      kind === 'invalid' ||
      parts.length !== expectedCount ||
      (expectedCount === 1 &&
       (number !== 1 || size !== expectedSize)))
    throw new ProbeError('OSS ListParts did not match the expected test part');
}

async function assertNoResidual(storage, accountId, key) {
  const prefix = `cloud-drive/${accountId}/`;
  const [uploads, objects] = await Promise.all([
    storage.listUploads({ prefix, 'max-uploads': 2 }),
    storage.list({ prefix, 'max-keys': 2 })
  ]);
  if ((uploads.uploads || []).length || uploads.isTruncated ||
      (objects.objects || []).length || objects.isTruncated)
    throw new ProbeError('Test account prefix still has OSS data');
  try { await storage.head(key); }
  catch (error) {
    if (error?.status === 404 || error?.code === 'NoSuchKey') return;
    throw new ProbeError(`Cannot verify test object removal: ${safeErrorCode(error)}`);
  }
  throw new ProbeError('Unexpected test object remains in OSS');
}

async function assertNoUploadAfterFailedInit(storage, key) {
  const uploads = await storage.listUploads({ prefix: key, 'max-uploads': 2 });
  if ((uploads.uploads || []).length || uploads.isTruncated)
    throw new ProbeError('Init may have succeeded; keep root-only recovery record');
}

function cloudEnvironment(credentials, bucket, region, secret) {
  return { ...credentials,
    MAICHAT_CLOUD_DRIVE_OSS_BUCKET: bucket,
    MAICHAT_CLOUD_DRIVE_OSS_REGION: region,
    MAICHAT_CLOUD_DRIVE_ENABLED: '1',
    MAICHAT_CLOUD_DRIVE_INTERNAL_KEY: secret };
}

function cloudStorage(credentials, bucket, region) {
  const OSS = require('ali-oss');
  return new OSS({ accessKeyId: credentials.MAICHAT_CLOUD_DRIVE_OSS_ACCESS_KEY_ID,
    accessKeySecret: credentials.MAICHAT_CLOUD_DRIVE_OSS_ACCESS_KEY_SECRET,
    bucket, region, authorizationV4: true, secure: true });
}

async function exercisePartChecks({ handler, secret, accountId, key, uploadId,
  bucket, region, storage, putPart, log, positiveOnly = false,
  payload = crypto.randomBytes(PART_BYTES) }) {
  if (!Buffer.isBuffer(payload) || payload.length !== PART_BYTES)
    throw new ProbeError('The smoke part must be exactly 8 MiB');
  const short = payload.subarray(0, 1024 * 1024);
  const shortMd5 = crypto.createHash('md5').update(short).digest('base64');
  const md5 = crypto.createHash('md5').update(payload).digest('base64');

  if (!positiveOnly) {
    const shortSigned = await callSigner(handler, secret, 'sign-part', accountId, key,
      { upload_id: uploadId, part_number: 1, expected_bytes: PART_BYTES,
        content_md5_base64: shortMd5 });
    const shortUrl = validateSignedPart(shortSigned, bucket, region, key,
      uploadId, PART_BYTES, shortMd5);
    const wrongLength = await putPart(shortUrl,
      { 'Content-Length': String(short.length), 'Content-MD5': shortMd5 }, short);
    log(`OSS wrong-length PUT: ${putOutcome(wrongLength)}`);
    if (wrongLength.status >= 200 && wrongLength.status < 300)
      throw new ProbeError('OSS accepted a part whose length differed from the signed length');
    await assertParts(storage, key, uploadId, 0, 0, log);
  }

  const signed = await callSigner(handler, secret, 'sign-part', accountId, key,
    { upload_id: uploadId, part_number: 1, expected_bytes: PART_BYTES,
      content_md5_base64: md5 });
  const url = validateSignedPart(signed, bucket, region, key, uploadId, PART_BYTES, md5);
  if (!positiveOnly) {
    const changed = Buffer.from(payload);
    changed[0] ^= 1;
    const wrongMd5 = await putPart(url, signed.upload_headers, changed);
    log(`OSS wrong-MD5 PUT: ${putOutcome(wrongMd5)}`);
    if (wrongMd5.status >= 200 && wrongMd5.status < 300)
      throw new ProbeError('OSS accepted a part whose bytes differed from Content-MD5');
    await assertParts(storage, key, uploadId, 0, 0, log);
  }

  const uploaded = await putPart(url, signed.upload_headers, payload);
  log(`OSS correct 8 MiB PUT: ${putOutcome(uploaded)}`);
  if (uploaded.status < 200 || uploaded.status >= 300)
    throw new ProbeError('OSS rejected the correctly signed part');
  await assertParts(storage, key, uploadId, 1, PART_BYTES, log);
  const signerParts = await callSigner(handler, secret, 'list-parts', accountId, key,
    { upload_id: uploadId });
  const verified = signerParts.parts;
  if (!Array.isArray(verified) || verified.length !== 1 ||
      verified[0]?.part_number !== 1 || verified[0]?.size_bytes !== PART_BYTES ||
      typeof verified[0]?.etag !== 'string' || !verified[0].etag)
    throw new ProbeError('Signer ListParts did not return the expected test part');
  log(`Signer ListParts: count=${verified.length} part_number=1 size_bytes=${PART_BYTES}`);
  log(`OSS ListParts: part 1 = ${PART_BYTES} bytes; SHA-256 ${
    crypto.createHash('sha256').update(payload).digest('hex')}`);
  if (positiveOnly) log('OSS positive-only PUT and ListParts: PASS');
  else log('OSS negative PUTs rejected: no stored part before the successful correct PUT');
  return { sha256: crypto.createHash('sha256').update(payload).digest('hex'),
    part: verified[0] };
}

async function exerciseCompleteRoundtrip({ handler, secret, accountId, key, uploadId,
  bucket, region, uploaded, getObject, writePhase, log }) {
  const completed = await callSigner(handler, secret, 'complete', accountId, key,
    { upload_id: uploadId, parts: [{ part_number: uploaded.part.part_number,
      etag: uploaded.part.etag }] });
  if (completed.completed !== true)
    throw new ProbeError('Signer did not confirm Multipart completion');
  await writePhase('completed');
  log('Signer CompleteMultipartUpload: HTTP 200; completed state synced');

  const head = await callSigner(handler, secret, 'verify', accountId, key,
    { expected_bytes: PART_BYTES, content_type: 'video/mp4' });
  if (head.exists !== true || head.size_bytes !== PART_BYTES ||
      head.content_type !== 'video/mp4')
    throw new ProbeError('Signer HEAD size or content type is wrong');
  log(`Signer HEAD: size_bytes=${PART_BYTES} content_type=video/mp4`);

  const signed = await callSigner(handler, secret, 'read', accountId, key);
  const readUrl = validateSignedRead(signed, bucket, region, key);
  const fetched = await getObject(readUrl, PART_BYTES);
  log(`OSS signed GET: ${putOutcome(fetched)} size_bytes=${fetched.size_bytes ?? 'unknown'}`);
  if (fetched.status !== 200 || fetched.size_bytes !== PART_BYTES ||
      fetched.sha256 !== uploaded.sha256)
    throw new ProbeError('Signed GET size or SHA-256 differs from the uploaded part');
  log(`OSS signed GET: SHA-256 ${fetched.sha256}`);

  await writePhase('deleting');
  const removed = await callSigner(handler, secret, 'delete', accountId, key);
  if (removed.deleted !== true) throw new ProbeError('Signer did not confirm object deletion');
  await writePhase('deleted');
  log('Signer delete: HTTP 200');
}

async function cleanupTestObject({ handler, secret, accountId, key, uploadIds,
  storage, log }) {
  for (const uploadId of uploadIds) {
    try { await callSigner(handler, secret, 'abort', accountId, key, { upload_id: uploadId }); }
    catch (error) {
      // Inventory below decides whether the failed abort actually left data behind.
      log(`Signer abort returned ${error instanceof ProbeError ?
        'non-200' : safeErrorCode(error)}; checking inventory`);
    }
  }
  const verified = await callSigner(handler, secret, 'verify', accountId, key,
    { expected_bytes: PART_BYTES, content_type: 'video/mp4' });
  if (verified.exists === true) {
    const deleted = await callSigner(handler, secret, 'delete', accountId, key);
    if (deleted.deleted !== true) throw new ProbeError('Signer could not delete the test object');
    log('Signer cleanup delete: HTTP 200');
  } else if (verified.exists !== false) {
    throw new ProbeError('Signer could not determine whether the test object exists');
  }
  await assertNoResidual(storage, accountId, key);
}

async function runSmoke({ bucket, region, credentials, recoveryDir = RECOVERY_DIR,
  makeStorage, makeHandler = createHandler, putPart = putSignedPart,
  getObject = getSignedObject, positiveOnly = false, completeRoundtrip = false,
  log = line => process.stdout.write(`${line}\n`) }) {
  assertTarget(bucket, region, bucket);
  await ensureRecoveryDir(recoveryDir);
  const storage = makeStorage ? makeStorage({ bucket, region, credentials }) :
    cloudStorage(credentials, bucket, region);
  await assertEmptyPrivateBucket(storage, bucket);
  const secret = crypto.randomBytes(32).toString('hex');
  const handler = makeHandler({ env: cloudEnvironment(credentials, bucket, region, secret) });
  const accountId = `test_${crypto.randomBytes(8).toString('hex')}`;
  const key = `cloud-drive/${accountId}/${new Date().toISOString().slice(0,10)}/${crypto.randomUUID()}.mp4`;
  const journal = path.join(recoveryDir, `${crypto.randomUUID()}.json`);
  const record = { bucket, region, account_id: accountId, object_key: key,
    upload_id: null, phase: 'init_pending', created_at: new Date().toISOString() };
  await writeRecovery(journal, record, true);
  let uploadId;
  let primaryError;
  let cleanupError;
  try {
    const initialized = await callSigner(handler, secret, 'init', accountId, key,
      { content_type: 'video/mp4' });
    uploadId = initialized.upload_id;
    if (typeof uploadId !== 'string' || !/^[A-Za-z0-9_+\/=\-]{1,512}$/.test(uploadId))
      throw new ProbeError('Signer returned an invalid upload ID');
    record.upload_id = uploadId;
    record.phase = 'uploading';
    await writeRecovery(journal, record);
    log('OSS init: HTTP 200; recovery record synced before PUT');
    const uploaded = await exercisePartChecks({ handler, secret, accountId, key, uploadId,
      bucket, region, storage, putPart, log,
      positiveOnly: positiveOnly || completeRoundtrip });
    if (completeRoundtrip) {
      await exerciseCompleteRoundtrip({ handler, secret, accountId, key,
        uploadId, bucket, region, uploaded, getObject,
        writePhase: async phase => { record.phase = phase; await writeRecovery(journal, record); },
        log });
    }
  } catch (error) { primaryError = error; }
  finally {
    try {
      if (uploadId) {
        await cleanupTestObject({ handler, secret, accountId, key,
          uploadIds: [uploadId], storage, log });
        log('OSS abort/delete and residual check: clean');
      } else {
        // An init could have succeeded immediately before its response failed.
        // The journal keeps the random key if the upload cannot be ruled out.
        await assertNoUploadAfterFailedInit(storage, key);
      }
    } catch (error) {
      cleanupError = new ProbeError(`Cleanup not proven; keep root-only recovery record. ${
        error instanceof ProbeError ? error.message : safeErrorCode(error)}`);
      if (primaryError) cleanupError.cause = primaryError;
    }
  }
  if (cleanupError) throw cleanupError;
  await clearRecovery(journal);
  if (primaryError) throw primaryError;
  log('Real OSS signer smoke: PASS');
}

async function runRecovery({ bucket, region, credentials, journalName,
  recoveryDir = RECOVERY_DIR, makeStorage, makeHandler = createHandler,
  log = line => process.stdout.write(`${line}\n`) }) {
  assertTarget(bucket, region, bucket);
  if (!/^[a-f0-9]{8}-[a-f0-9]{4}-4[a-f0-9]{3}-[89ab][a-f0-9]{3}-[a-f0-9]{12}\.json$/.test(journalName || ''))
    throw new ProbeError('Recovery requires a generated journal basename');
  await ensureRecoveryDir(recoveryDir);
  const journal = path.join(recoveryDir, journalName);
  const record = await readRecovery(journal, bucket, region);
  const storage = makeStorage ? makeStorage({ bucket, region, credentials }) :
    cloudStorage(credentials, bucket, region);
  await assertPrivateBucketPolicy(storage, bucket);
  const secret = crypto.randomBytes(32).toString('hex');
  const handler = makeHandler({ env: cloudEnvironment(credentials, bucket, region, secret) });
  const uploads = await storage.listUploads({ prefix: record.object_key, 'max-uploads': 2 });
  const ids = recoveryUploadIds(record, uploads);
  await cleanupTestObject({ handler, secret, accountId: record.account_id,
    key: record.object_key, uploadIds: ids, storage, log });
  await clearRecovery(journal);
  log(`OSS recovery: ${ids.size} upload(s) checked; test account prefix clean`);
}

async function main(args = process.argv.slice(2)) {
  const { bucket, region, envFile, positiveOnly, completeRoundtrip, recover } = parseArgs(args);
  const credentials = await readServerCredentials(envFile);
  if (recover) await runRecovery({ bucket, region, credentials, journalName: recover });
  else await runSmoke({ bucket, region, credentials, positiveOnly, completeRoundtrip });
}

if (require.main === module) {
  main().catch(error => {
    const detail = error instanceof ProbeError ? error.message : safeErrorCode(error);
    process.stderr.write(`Real OSS signer smoke: FAIL (${detail})\n`);
    process.exitCode = 1;
  });
}

module.exports = { ProbeError, assertTarget, parseArgs, readServerCredentials,
  ensureRecoveryDir,
  writeRecovery, clearRecovery, readRecovery, validateRecoveryRecord,
  recoveryUploadIds, validateSignedPart,
  assertPrivateBucketPolicy, assertEmptyPrivateBucket,
  putSignedPart, putOutcome, validateSignedRead, getSignedObject,
  exercisePartChecks, exerciseCompleteRoundtrip, cleanupTestObject, assertNoResidual,
  assertNoUploadAfterFailedInit,
  runSmoke, runRecovery, main };
