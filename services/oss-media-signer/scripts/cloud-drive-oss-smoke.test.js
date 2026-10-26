'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs/promises');
const os = require('node:os');
const path = require('node:path');
const { EventEmitter } = require('node:events');
const { parseArgs, validateSignedPart, assertEmptyPrivateBucket,
  assertNoResidual, assertNoUploadAfterFailedInit, writeRecovery,
  clearRecovery, putOutcome, exercisePartChecks, validateRecoveryRecord,
  recoveryUploadIds, putSignedPart, exerciseCompleteRoundtrip,
  cleanupTestObject } = require('./cloud-drive-oss-smoke');

const BUCKET = 'maichat-private-cloud-drive-gz-20261025-f346b709';
const REGION = 'oss-cn-guangzhou';
const ACCOUNT = 'test_1234567890abcdef';
const KEY = `cloud-drive/${ACCOUNT}/2026-10-25/123e4567-e89b-42d3-a456-426614174000.mp4`;

test('requires explicit new bucket confirmation and refuses the existing media bucket', () => {
  const parsed = parseArgs([`--bucket=${BUCKET}`, `--region=${REGION}`,
    '--env-file=/etc/maichat/media.env', `--confirm-new-empty-bucket=${BUCKET}`]);
  assert.equal(parsed.bucket, BUCKET);
  assert.throws(() => parseArgs([`--bucket=${BUCKET}`, `--region=${REGION}`,
    '--env-file=/etc/maichat/media.env', '--confirm-new-empty-bucket=other-bucket']));
  assert.throws(() => parseArgs(['--bucket=maichat-ai-video-resource',
    `--region=${REGION}`, '--env-file=/etc/maichat/media.env',
    '--confirm-new-empty-bucket=maichat-ai-video-resource']));
  assert.equal(parseArgs([`--bucket=${BUCKET}`, `--region=${REGION}`,
    '--env-file=/etc/maichat/media.env', `--confirm-new-empty-bucket=${BUCKET}`,
    '--positive-only']).positiveOnly, true);
  assert.equal(parseArgs([`--bucket=${BUCKET}`, `--region=${REGION}`,
    '--env-file=/etc/maichat/media.env', `--confirm-new-empty-bucket=${BUCKET}`,
    '--recover=123e4567-e89b-42d3-a456-426614174000.json']).recover,
  '123e4567-e89b-42d3-a456-426614174000.json');
  assert.throws(() => parseArgs([`--bucket=${BUCKET}`, `--region=${REGION}`,
    '--env-file=/etc/maichat/media.env', `--confirm-new-empty-bucket=${BUCKET}`,
    '--recover=../../media.env']));
  assert.equal(parseArgs([`--bucket=${BUCKET}`, `--region=${REGION}`,
    '--env-file=/etc/maichat/media.env', `--confirm-new-empty-bucket=${BUCKET}`,
    '--complete-roundtrip']).completeRoundtrip, true);
  assert.throws(() => parseArgs([`--bucket=${BUCKET}`, `--region=${REGION}`,
    '--env-file=/etc/maichat/media.env', `--confirm-new-empty-bucket=${BUCKET}`,
    '--complete-roundtrip', '--positive-only']));
});

test('presigned part is accepted only for the exact bucket, key, upload and length', () => {
  const uploadId = 'OSSupload_123';
  const md5 = 'AAAAAAAAAAAAAAAAAAAAAA==';
  const url = `https://${BUCKET}.${REGION}.aliyuncs.com/${KEY}` +
    '?partNumber=1&uploadId=OSSupload_123&x-oss-additional-headers=content-length';
  const signed = { upload_url: url,
    upload_headers: { 'Content-Length': '8388608', 'Content-MD5': md5 } };
  assert.equal(validateSignedPart(signed, BUCKET, REGION, KEY, uploadId,
    8388608, md5).hostname, `${BUCKET}.${REGION}.aliyuncs.com`);
  assert.throws(() => validateSignedPart(signed, 'other-bucket', REGION,
    KEY, uploadId, 8388608, md5));
  assert.throws(() => validateSignedPart(signed, BUCKET, REGION,
    KEY, 'other-upload', 8388608, md5));
  assert.throws(() => validateSignedPart(signed, BUCKET, REGION,
    KEY, uploadId, 1048576, md5));
});

test('a private but nonempty bucket is rejected before any init', async () => {
  const prefixes = [];
  const storage = {
    getBucketACL: async () => ({ acl: 'private' }),
    getBucketVersioning: async () => ({ versionStatus: undefined }),
    getBucketLifecycle: async () => ({ rules: [{ prefix: 'cloud-drive/',
      status: 'Enabled', abortMultipartUpload: { days: '8' } }] }),
    list: async options => { prefixes.push(options.prefix);
      return { objects: [{ name: 'cloud-drive/existing.mp4' }] }; },
    listUploads: async options => { prefixes.push(options.prefix);
      return { uploads: [] }; }
  };
  await assert.rejects(assertEmptyPrivateBucket(storage, BUCKET), /not empty/);
  assert.deepEqual(prefixes, ['cloud-drive/', 'cloud-drive/']);
  storage.list = async () => ({ objects: [] });
  storage.listUploads = async () => ({ uploads: [{ name: 'still-uploading' }] });
  await assert.rejects(assertEmptyPrivateBucket(storage, BUCKET), /not empty/);
  storage.listUploads = async () => ({ uploads: [] });
  storage.getBucketACL = async () => ({ acl: 'public-read' });
  await assert.rejects(assertEmptyPrivateBucket(storage, BUCKET), /private/);
});

test('versioning and multipart lifecycle must be safe before writes', async () => {
  const storage = {
    getBucketACL: async () => ({ acl: 'private' }),
    getBucketVersioning: async () => ({ versionStatus: 'Enabled' }),
    getBucketLifecycle: async () => ({ rules: [{ prefix: 'cloud-drive/',
      status: 'Enabled', abortMultipartUpload: { days: '8' } }] }),
    list: async () => ({ objects: [] }),
    listUploads: async () => ({ uploads: [] })
  };
  await assert.rejects(assertEmptyPrivateBucket(storage, BUCKET), /versioning/);
  storage.getBucketVersioning = async () => ({ versionStatus: undefined });
  storage.getBucketLifecycle = async () => ({ rules: [] });
  await assert.rejects(assertEmptyPrivateBucket(storage, BUCKET), /8-day/);
  storage.getBucketLifecycle = async () => ({ rules: [
    { prefix: '', status: 'Enabled', abortMultipartUpload: { days: '1' } },
    { prefix: 'cloud-drive/', status: 'Enabled', abortMultipartUpload: { days: '8' } }
  ] });
  await assert.rejects(assertEmptyPrivateBucket(storage, BUCKET), /8-day/);
});

test('post-abort inventory checks the whole unique test account prefix', async () => {
  const prefixes = [];
  const storage = {
    listUploads: async options => { prefixes.push(options.prefix); return { uploads: [] }; },
    list: async options => { prefixes.push(options.prefix); return { objects: [] }; },
    head: async () => { throw { status: 404 }; }
  };
  await assertNoResidual(storage, ACCOUNT, KEY);
  assert.deepEqual(prefixes, [`cloud-drive/${ACCOUNT}/`, `cloud-drive/${ACCOUNT}/`]);
  storage.list = async () => ({ objects: [{ name: `${KEY}.unexpected` }] });
  await assert.rejects(assertNoResidual(storage, ACCOUNT, KEY), /still has OSS data/);
});

test('failed init keeps its journal if OSS has a possible upload', async () => {
  await assertNoUploadAfterFailedInit({ listUploads: async () => ({ uploads: [] }) }, KEY);
  await assert.rejects(assertNoUploadAfterFailedInit({ listUploads: async () =>
    ({ uploads: [{ name: KEY, uploadId: 'possibly-live' }] }) }, KEY), /keep root-only/);
});

test('recovery record is mode 0600 and updates atomically before PUT', async () => {
  const directory = await fs.mkdtemp(path.join(os.tmpdir(), 'maichat-oss-journal-'));
  const filename = path.join(directory, 'recovery.json');
  try {
    await writeRecovery(filename, { object_key: KEY, upload_id: null }, true);
    await writeRecovery(filename, { object_key: KEY, upload_id: 'real-upload-id' });
    const stat = await fs.stat(filename);
    assert.equal(stat.mode & 0o777, 0o600);
    assert.equal(JSON.parse(await fs.readFile(filename, 'utf8')).upload_id,
      'real-upload-id');
    await clearRecovery(filename);
    await assert.rejects(fs.stat(filename), { code: 'ENOENT' });
  } finally { await fs.rm(directory, { recursive: true, force: true }); }
});

test('MD5 transport reset is only accepted after zero parts and a correct PUT succeeds', async () => {
  const uploadId = 'OSSupload_123';
  const logs = [];
  let putCount = 0;
  let partPresent = false;
  const handler = async event => {
    const body = JSON.parse(event.body);
    if (event.rawPath.endsWith('/list-parts'))
      return { statusCode: 200, body: JSON.stringify({ parts: [{ part_number: 1,
        size_bytes: 8388608, etag: 'must-not-appear-in-log' }] }) };
    const headers = { 'Content-Length': String(body.expected_bytes),
      'Content-MD5': body.content_md5_base64 };
    const url = `https://${BUCKET}.${REGION}.aliyuncs.com/${KEY}` +
      `?partNumber=1&uploadId=${uploadId}&x-oss-additional-headers=content-length`;
    return { statusCode: 200, body: JSON.stringify({ upload_url: url,
      upload_headers: headers }) };
  };
  const storage = { listParts: async () => ({ res: { status: 200 }, parts: partPresent ?
    { PartNumber: '1', Size: '8388608', ETag: 'must-not-appear-in-log' } : [] }) };
  const putPart = async () => {
    putCount += 1;
    if (putCount === 1) return { status: 403, code: 'SignatureDoesNotMatch' };
    if (putCount === 2) return { status: 0, transport_code: 'ECONNRESET', phase: 'send' };
    partPresent = true;
    return { status: 200, code: '' };
  };
  await exercisePartChecks({ handler, secret: 'test', accountId: ACCOUNT, key: KEY,
    uploadId, bucket: BUCKET, region: REGION, storage, putPart,
    log: line => logs.push(line), payload: Buffer.alloc(8 * 1024 * 1024) });
  assert.equal(putCount, 3);
  assert.ok(logs.some(line => line.includes('transport ECONNRESET phase=send')));
  assert.ok(logs.some(line => line.includes('HTTP 200 parts=object length=1') &&
    line.includes('part_number=1 size_bytes=8388608')));
  assert.ok(logs.every(line => !line.includes('must-not-appear-in-log') &&
    !line.includes(KEY) && !line.includes(uploadId)));
  assert.ok(logs.some(line => line.includes('negative PUTs rejected')));
  assert.equal(putOutcome({ status: 0, transport_code: 'ECONNRESET',
    phase: 'await_response' }), 'transport ECONNRESET phase=await_response');
});

test('MD5 reset does not pass when the correct PUT fails too', async () => {
  const uploadId = 'OSSupload_123';
  let putCount = 0;
  const handler = async event => {
    const body = JSON.parse(event.body);
    return { statusCode: 200, body: JSON.stringify({ upload_url:
      `https://${BUCKET}.${REGION}.aliyuncs.com/${KEY}` +
      `?partNumber=1&uploadId=${uploadId}&x-oss-additional-headers=content-length`,
    upload_headers: { 'Content-Length': String(body.expected_bytes),
      'Content-MD5': body.content_md5_base64 } }) };
  };
  const storage = { listParts: async () => ({ parts: [] }) };
  const putPart = async () => {
    putCount += 1;
    if (putCount === 1) return { status: 403, code: 'SignatureDoesNotMatch' };
    return { status: 0, transport_code: 'ECONNRESET', phase: 'await_response' };
  };
  await assert.rejects(exercisePartChecks({ handler, secret: 'test',
    accountId: ACCOUNT, key: KEY, uploadId, bucket: BUCKET, region: REGION,
    storage, putPart, log: () => {}, payload: Buffer.alloc(8 * 1024 * 1024) }),
  /correctly signed part/);
  assert.equal(putCount, 3);
});

test('positive-only skips deliberate failures and checks signer ListParts', async () => {
  const uploadId = 'OSSupload_123';
  const logs = [];
  let putCount = 0;
  const handler = async event => {
    if (event.rawPath.endsWith('/list-parts'))
      return { statusCode: 200, body: JSON.stringify({ parts: [{ part_number: 1,
        size_bytes: 8388608, etag: 'must-not-appear-in-log' }] }) };
    const body = JSON.parse(event.body);
    return { statusCode: 200, body: JSON.stringify({ upload_url:
      `https://${BUCKET}.${REGION}.aliyuncs.com/${KEY}` +
      `?partNumber=1&uploadId=${uploadId}&x-oss-additional-headers=content-length`,
    upload_headers: { 'Content-Length': String(body.expected_bytes),
      'Content-MD5': body.content_md5_base64 } }) };
  };
  const storage = { listParts: async () => ({ res: { status: 200 },
    parts: { PartNumber: '1', Size: '8388608', ETag: 'must-not-appear-in-log' } }) };
  await exercisePartChecks({ handler, secret: 'test', accountId: ACCOUNT,
    key: KEY, uploadId, bucket: BUCKET, region: REGION, storage,
    putPart: async () => { putCount += 1; return { status: 200 }; },
    log: line => logs.push(line), positiveOnly: true,
    payload: Buffer.alloc(8 * 1024 * 1024) });
  assert.equal(putCount, 1);
  assert.ok(logs.some(line => line.includes('Signer ListParts: count=1')));
  assert.ok(logs.every(line => !line.includes('must-not-appear-in-log')));
});

test('recovery only accepts the exact new bucket and generated test account key', () => {
  const record = { bucket: BUCKET, region: REGION, account_id: ACCOUNT,
    object_key: KEY, upload_id: 'OSSupload_123' };
  assert.doesNotThrow(() => validateRecoveryRecord(record, BUCKET, REGION));
  assert.throws(() => validateRecoveryRecord({ ...record,
    bucket: 'maichat-ai-video-resource' }, BUCKET, REGION));
  assert.throws(() => validateRecoveryRecord({ ...record,
    object_key: KEY.replace('cloud-drive/', 'avatars/') }, BUCKET, REGION));
  assert.throws(() => validateRecoveryRecord({ ...record,
    account_id: 'real_user' }, BUCKET, REGION));
  assert.deepEqual([...recoveryUploadIds(record, { uploads: [
    { name: KEY, uploadId: 'OSSupload_123' }] })], ['OSSupload_123']);
  assert.throws(() => recoveryUploadIds(record, { uploads: [
    { name: `${KEY}.other`, uploadId: 'OSSupload_123' }] }));
});

test('direct TLSSocket EPIPE settles the PUT without an unhandled error', async () => {
  const socket = new EventEmitter();
  const request = new EventEmitter();
  request.end = () => {
    request.emit('socket', socket);
    socket.emit('error', Object.assign(new Error('closed early'), { code: 'EPIPE' }));
  };
  const result = await putSignedPart(new URL(`https://${BUCKET}.${REGION}.aliyuncs.com/${KEY}`),
    { 'Content-Length': '1' }, Buffer.from([0]), () => request);
  assert.deepEqual(result, { status: 0, transport_code: 'EPIPE', phase: 'connect' });
});

test('complete roundtrip checks signer HEAD, signed GET hash and delete', async () => {
  const uploadId = 'OSSupload_123';
  const sha256 = 'a'.repeat(64);
  const etag = '"' + 'b'.repeat(32) + '"';
  const phases = [];
  const actions = [];
  const logs = [];
  const handler = async event => {
    const action = event.rawPath.split('/').at(-1);
    actions.push(action);
    const response = action === 'complete' ? { completed: true } :
      action === 'verify' ? { exists: true, size_bytes: 8388608,
        content_type: 'video/mp4' } :
      action === 'read' ? { read_url:
        `https://${BUCKET}.${REGION}.aliyuncs.com/${KEY}?signed=private`,
        read_expires_in: 60 } :
      action === 'delete' ? { deleted: true } : {};
    return { statusCode: 200, body: JSON.stringify(response) };
  };
  await exerciseCompleteRoundtrip({ handler, secret: 'test', accountId: ACCOUNT,
    key: KEY, uploadId, bucket: BUCKET, region: REGION,
    uploaded: { sha256, part: { part_number: 1, size_bytes: 8388608,
      etag } }, getObject: async () => ({ status: 200, size_bytes: 8388608, sha256 }),
    writePhase: async phase => { phases.push(phase); },
    log: line => logs.push(line) });
  assert.deepEqual(actions, ['complete', 'verify', 'read', 'delete']);
  assert.deepEqual(phases, ['completed', 'deleting', 'deleted']);
  assert.ok(logs.some(line => line.includes('Signer HEAD: size_bytes=8388608')));
  assert.ok(logs.every(line => !line.includes(etag) && !line.includes(KEY) &&
    !line.includes('signed=private')));
});

test('cleanup after a completed object aborts upload and deletes only test object', async () => {
  const actions = [];
  const handler = async event => {
    const action = event.rawPath.split('/').at(-1);
    actions.push(action);
    const body = JSON.parse(event.body);
    assert.equal(body.object_key, KEY);
    return { statusCode: 200, body: JSON.stringify(action === 'verify' ?
      { exists: true } : action === 'delete' ? { deleted: true } : { aborted: true }) };
  };
  const storage = {
    listUploads: async () => ({ uploads: [] }),
    list: async () => ({ objects: [] }),
    head: async () => { throw { status: 404 }; }
  };
  await cleanupTestObject({ handler, secret: 'test', accountId: ACCOUNT,
    key: KEY, uploadIds: ['OSSupload_123'], storage, log: () => {} });
  assert.deepEqual(actions, ['abort', 'verify', 'delete']);
});
