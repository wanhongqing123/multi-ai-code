// Operator-only cleanup for account-scoped OSS keys issued after the privacy migration.
'use strict';

const PREFIXES = ['ark-asset-inputs', 'avatars', 'kling-inputs',
  'minimax-inputs', 'seedream-inputs', 'wan-inputs'];

async function purgeAccountAssets(client, accountId, execute = false,
                                  includeVideoCircle = false,
                                  cloudClient = null) {
  if (typeof accountId !== 'string' || !/^[a-z][a-z0-9_-]{2,31}$/.test(accountId))
    throw new Error('Invalid MaiChat account ID');
  const result = {};
  const roots = [...PREFIXES.map(root => ({ root, storage:client })),
    ...(includeVideoCircle ? [{ root:'video-circle',storage:client }] : []),
    ...(cloudClient ? [{ root:'cloud-drive',storage:cloudClient },
      { root:'session-attachments',storage:cloudClient }] : [])];
  for (const { root,storage } of roots) {
    const prefix = `${root}/${accountId}/`;
    let marker;
    let count = 0;
    while (true) {
      const page = await storage.list({ prefix,
        ...(marker ? { marker } : {}), 'max-keys': 1000 });
      const keys = (page.objects || []).map(object => object.name);
      if (keys.some(key => !key.startsWith(prefix)))
        throw new Error('OSS returned an object outside the requested account prefix');
      if (!keys.length) break;
      count += keys.length;
      if (execute) await storage.deleteMulti(keys, { quiet: true });
      const next = keys.at(-1);
      if (next === marker) throw new Error('OSS pagination did not advance');
      marker = next;
      if (keys.length < 1000) break;
    }
    result[root] = count;
  }
  const multipartCounts = {};
  if (cloudClient) {
    for (const root of ['cloud-drive','session-attachments']) {
    const prefix = `${root}/${accountId}/`;
    let multipartCount = 0;
    let keyMarker;
    let uploadIdMarker;
    while (true) {
      const page = await cloudClient.listUploads({ prefix, 'max-uploads':1000,
        ...(keyMarker ? { 'key-marker':keyMarker } : {}),
        ...(uploadIdMarker ? { 'upload-id-marker':uploadIdMarker } : {}) });
      const uploads = page.uploads || [];
      if (uploads.some(upload => !upload.name.startsWith(prefix) ||
          typeof upload.uploadId !== 'string' || !upload.uploadId))
        throw new Error('OSS returned a multipart upload outside the account prefix');
      multipartCount += uploads.length;
      if (execute) {
        for (const upload of uploads)
          await cloudClient.abortMultipartUpload(upload.name,upload.uploadId);
      }
      if (page.isTruncated !== true && page.isTruncated !== 'true') break;
      if (!page.nextKeyMarker || !page.nextUploadIdMarker ||
          (page.nextKeyMarker === keyMarker &&
           page.nextUploadIdMarker === uploadIdMarker))
        throw new Error('OSS multipart pagination did not advance');
      keyMarker = page.nextKeyMarker;
      uploadIdMarker = page.nextUploadIdMarker;
    }
    multipartCounts[root] = multipartCount;
    }
  }
  return { account_id: accountId, execute, object_counts: result,
    ...(cloudClient ? { multipart_upload_counts:multipartCounts } : {}) };
}

async function main(args = process.argv.slice(2), env = process.env) {
  const includeVideoCircle = args.includes('--include-video-circle');
  const includeCloudDrive = args.includes('--include-cloud-drive');
  const filtered = args.filter(argument =>
    argument !== '--include-video-circle' && argument !== '--include-cloud-drive');
  const execute = filtered[0] === '--execute';
  const accountId = execute ? filtered[1] : filtered[0];
  const confirmed = execute ? filtered[2] : accountId;
  if (filtered.length !== (execute ? 3 : 1) ||
      args.length !== filtered.length + (includeVideoCircle ? 1 : 0) +
        (includeCloudDrive ? 1 : 0) ||
      accountId !== confirmed ||
      !env.MAICHAT_OSS_BUCKET || !env.MAICHAT_OSS_REGION ||
      !env.ALIBABA_CLOUD_ACCESS_KEY_ID || !env.ALIBABA_CLOUD_ACCESS_KEY_SECRET ||
      (includeCloudDrive &&
       (!env.MAICHAT_CLOUD_DRIVE_OSS_BUCKET ||
        env.MAICHAT_CLOUD_DRIVE_OSS_BUCKET === env.MAICHAT_OSS_BUCKET ||
        !env.MAICHAT_CLOUD_DRIVE_OSS_REGION ||
        !env.MAICHAT_CLOUD_DRIVE_OSS_ACCESS_KEY_ID ||
        !env.MAICHAT_CLOUD_DRIVE_OSS_ACCESS_KEY_SECRET)))
    throw new Error('Use [--include-video-circle] [--include-cloud-drive] ' +
      '[--execute] <account-id> ' +
      '[same-account-id] with OSS credentials');
  const OSS = require('ali-oss');
  const client = new OSS({ accessKeyId: env.ALIBABA_CLOUD_ACCESS_KEY_ID,
    accessKeySecret: env.ALIBABA_CLOUD_ACCESS_KEY_SECRET,
    bucket: env.MAICHAT_OSS_BUCKET, region: env.MAICHAT_OSS_REGION,
    authorizationV4: true, secure: true });
  const cloudClient = includeCloudDrive ? new OSS({
    accessKeyId:env.MAICHAT_CLOUD_DRIVE_OSS_ACCESS_KEY_ID,
    accessKeySecret:env.MAICHAT_CLOUD_DRIVE_OSS_ACCESS_KEY_SECRET,
    bucket:env.MAICHAT_CLOUD_DRIVE_OSS_BUCKET,
    region:env.MAICHAT_CLOUD_DRIVE_OSS_REGION,
    authorizationV4:true,secure:true }) : null;
  const result = await purgeAccountAssets(client, accountId, execute,
    includeVideoCircle,cloudClient);
  process.stdout.write(`${JSON.stringify(result)}\n`);
}

if (require.main === module) {
  main().catch(error => {
    process.stderr.write(`${error.message}\n`);
    process.exitCode = 1;
  });
}

module.exports = { purgeAccountAssets, main };
