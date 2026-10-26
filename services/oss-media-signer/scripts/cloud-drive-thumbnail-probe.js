// Opt-in real OSS processing probe. Never upload or read a user's private file.
'use strict';

const crypto = require('node:crypto');
const fs = require('node:fs/promises');
const https = require('node:https');
const path = require('node:path');
const OSS = require('ali-oss');
const { createHandler } = require('../index');
const { ProbeError,readServerCredentials,ensureRecoveryDir,
  writeRecovery,clearRecovery } = require('./cloud-drive-oss-smoke');

const RECOVERY_DIR = '/var/lib/maichat/oss-smoke-recovery';
const MAX_PREVIEW_BYTES = 2_000_000;
const FORMATS = Object.freeze({
  video:{ suffix:'.mp4',contentType:'video/mp4',maxBytes:10_000_000 },
  image:{ suffix:'.png',contentType:'image/png',maxBytes:2_000_000 },
  heic:{ suffix:'.heic',contentType:'image/heic',maxBytes:2_000_000 }
});

function args(values) {
  const options = {};
  for (const arg of values) {
    if (arg === '--run' && !options.run) { options.run = true; continue; }
    const match = /^--(bucket|region|env-file|account-id|confirm-account|video-file|image-file|heic-file|recover)=(.+)$/.exec(arg);
    if (!match || options[match[1]]) throw new ProbeError('Invalid thumbnail probe arguments');
    options[match[1]] = match[2];
  }
  if (!options.run || !options.bucket || !options.region || !options['env-file'] ||
      !options['account-id'] || !options['confirm-account'] ||
      options['account-id'] !== options['confirm-account'] ||
      !/^[a-z][a-z0-9_-]{2,31}$/.test(options['account-id']) ||
      !/^[a-z0-9][a-z0-9-]{1,61}[a-z0-9]$/.test(options.bucket) ||
      options.bucket === 'maichat-ai-video-resource' ||
      !/^oss-[a-z0-9-]+$/.test(options.region) ||
      (!options.recover && (!options['video-file'] || !options['image-file'])) ||
      (options.recover && !/^[a-f0-9-]{36}\.thumbnail-probe\.json$/.test(options.recover)))
    throw new ProbeError('Missing or unsafe thumbnail probe arguments');
  return options;
}

async function source(filename,kind) {
  const expected = FORMATS[kind];
  const stat = await fs.lstat(filename);
  if (!stat.isFile() || stat.isSymbolicLink() ||
      path.extname(filename).toLowerCase() !== expected.suffix ||
      stat.size < 1 || stat.size > expected.maxBytes)
    throw new ProbeError(`Invalid disposable ${kind} fixture`);
  return { filename,size:stat.size,...expected };
}

function safeKey(accountId,kind,uuid = crypto.randomUUID()) {
  return `cloud-drive/${accountId}/${new Date().toISOString().slice(0,10)}/`+
    `${uuid}${FORMATS[kind].suffix}`;
}

async function signedPreview(url) {
  return new Promise((resolve,reject) => {
    const request = https.get(url,{ timeout:90_000 },response => {
      const chunks = [];
      let size = 0;
      response.on('data',chunk => {
        size += chunk.length;
        if (size > MAX_PREVIEW_BYTES) {
          request.destroy(new ProbeError('OSS thumbnail exceeded the response cap'));
          return;
        }
        chunks.push(chunk);
      });
      response.on('end',() => {
        const data = Buffer.concat(chunks);
        if (response.statusCode !== 200 ||
            !String(response.headers['content-type'] || '').startsWith('image/jpeg') ||
            data.length < 4 || data[0] !== 0xff || data[1] !== 0xd8 ||
            data.at(-2) !== 0xff || data.at(-1) !== 0xd9)
          reject(new ProbeError(`OSS thumbnail is not JPEG (HTTP ${response.statusCode})`));
        else resolve({ bytes:data.length,sha256:crypto.createHash('sha256')
          .update(data).digest('hex') });
      });
      response.on('error',reject);
    });
    request.on('timeout',() => request.destroy(new ProbeError('OSS thumbnail timed out')));
    request.on('error',reject);
  });
}

function safeFailure(error) {
  return error instanceof ProbeError ? error.message :
    `OSS request failed${Number.isInteger(error?.status) ? ` HTTP ${error.status}` : ''}`;
}

async function cleanup(storage,keys) {
  for (const key of keys) {
    await storage.delete(key);
    try { await storage.head(key); }
    catch (error) {
      if (error?.status === 404 || error?.code === 'NoSuchKey') continue;
      throw error;
    }
    throw new ProbeError('Disposable object remained after DELETE');
  }
}

async function run(input = process.argv.slice(2),log = line => process.stdout.write(`${line}\n`)) {
  const options = args(input);
  await ensureRecoveryDir(RECOVERY_DIR);
  const credentials = await readServerCredentials(options['env-file']);
  const storage = new OSS({ bucket:options.bucket,region:options.region,
    accessKeyId:credentials.MAICHAT_CLOUD_DRIVE_OSS_ACCESS_KEY_ID,
    accessKeySecret:credentials.MAICHAT_CLOUD_DRIVE_OSS_ACCESS_KEY_SECRET,
    authorizationV4:true,secure:true });
  if (options.recover) {
    const journal = path.join(RECOVERY_DIR,options.recover);
    const stat = await fs.lstat(journal);
    if (!stat.isFile() || stat.isSymbolicLink() || stat.uid !== 0 ||
        (stat.mode & 0o077) !== 0 || stat.size > 4096)
      throw new ProbeError('Recovery journal must be a small root-only file');
    const record = JSON.parse(await fs.readFile(journal,'utf8'));
    if (record.bucket !== options.bucket || record.region !== options.region ||
        record.account_id !== options['account-id'] ||
        !Array.isArray(record.keys) || record.keys.length < 1 || record.keys.length > 3 ||
        record.keys.some(key => !/^cloud-drive\/[a-z][a-z0-9_-]{2,31}\/\d{4}-\d{2}-\d{2}\/[a-f0-9-]{36}\.(mp4|png|heic)$/.test(key) ||
          !key.startsWith(`cloud-drive/${record.account_id}/`)))
      throw new ProbeError('Recovery record does not match the exact test account');
    await cleanup(storage,record.keys);
    await clearRecovery(journal);
    log('Disposable thumbnail object recovery: PASS');
    return;
  }
  const assets = await Promise.all([
    source(options['video-file'],'video'),source(options['image-file'],'image'),
    options['heic-file'] ? source(options['heic-file'],'heic') : null
  ]);
  const selected = assets.filter(Boolean);
  const record = { bucket:options.bucket,region:options.region,
    account_id:options['account-id'],
    keys:selected.map((_,index) => safeKey(options['account-id'],
      ['video','image','heic'][index])) };
  const journal = path.join(RECOVERY_DIR,
    `${crypto.randomUUID()}.thumbnail-probe.json`);
  await writeRecovery(journal,record,true);
  const secret = crypto.randomBytes(32).toString('hex');
  const handler = createHandler({ env:{
    MAICHAT_CLOUD_DRIVE_ENABLED:'1',MAICHAT_CLOUD_DRIVE_INTERNAL_KEY:secret,
    MAICHAT_CLOUD_DRIVE_OSS_BUCKET:options.bucket,
    MAICHAT_CLOUD_DRIVE_OSS_REGION:options.region,
    ...credentials
  } });
  let originalError;
  try {
    for (let index=0; index<selected.length; index++) {
      const asset = selected[index];
      const key = record.keys[index];
      await storage.put(key,asset.filename,{ mime:asset.contentType,
        headers:{ 'x-oss-object-acl':'private',
          'x-oss-forbid-overwrite':'true' } });
      const response = await handler({ rawPath:'/cloud-drive/thumbnail',
        httpMethod:'POST',headers:{ 'X-MaiChat-Cloud-Drive-Key':secret },
        body:JSON.stringify({ account_id:options['account-id'],object_key:key }) });
      if (response.statusCode !== 200)
        throw new ProbeError(`Thumbnail signer returned HTTP ${response.statusCode}`);
      const signed = JSON.parse(response.body);
      const url = new URL(signed.thumbnail_url);
      if (signed.thumbnail_expires_in !== 60 ||
          url.searchParams.get('x-oss-process') !==
            (asset.contentType.startsWith('image/')
              ? 'image/resize,m_lfit,w_320,h_320/format,jpg'
              : 'video/snapshot,t_0,f_jpg,w_320'))
        throw new ProbeError('Thumbnail signature omitted its fixed transform');
      const preview = await signedPreview(signed.thumbnail_url);
      log(`${asset.contentType} private thumbnail: HTTP 200 JPEG ${preview.bytes} B`);
    }
  } catch (error) { originalError = error; }
  try {
    await cleanup(storage,record.keys);
    await clearRecovery(journal);
  } catch (error) {
    throw new ProbeError(`Cleanup not proven; keep ${path.basename(journal)} (${safeFailure(error)})`);
  }
  if (originalError) throw originalError;
  log('Real private OSS thumbnail probe: PASS; disposable objects deleted');
}

if (require.main === module) run().catch(error => {
  process.stderr.write(`Real private OSS thumbnail probe: FAIL (${safeFailure(error)})\n`);
  process.exitCode = 1;
});

module.exports = { args,source,safeKey,signedPreview,run };
