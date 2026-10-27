#!/usr/bin/env node
'use strict';

// PutBucketLifecycle replaces the entire rule list. Preserve existing rules,
// refuse overlap, and require an explicit apply on the reviewed private bucket.
const BUCKET = 'maichat-private-cloud-drive-gz-20261025-f346b709';
const REGION = 'oss-cn-guangzhou';
const PREFIX = 'session-attachments/';

function plan(rules) {
  if (!Array.isArray(rules) || !rules.some(rule =>
      rule.prefix === 'cloud-drive/' && rule.status === 'Enabled' &&
      Number(rule.abortMultipartUpload?.days) === 8))
    throw new Error('Existing cloud-drive 8-day rule was not verified');
  const existing = rules.find(rule => rule.prefix === PREFIX);
  if (existing) {
    if (existing.status === 'Enabled' &&
        Number(existing.abortMultipartUpload?.days) === 8 &&
        Object.keys(existing).every(key =>
          ['id','prefix','status','abortMultipartUpload'].includes(key)))
      return { status:'already-configured',rules };
    throw new Error('Session attachment lifecycle rule differs from the reviewed rule');
  }
  if (rules.some(rule => rule.abortMultipartUpload &&
      (PREFIX.startsWith(rule.prefix) || rule.prefix.startsWith(PREFIX))))
    throw new Error('An overlapping multipart lifecycle rule already exists');
  return { status:'ready',rules:[...rules,{
    id:'maichat-session-attachments-abort-multipart-8d',
    prefix:PREFIX,status:'Enabled',abortMultipartUpload:{ days:8 }
  }] };
}

async function upgrade(storage, apply = false) {
  const first = (await storage.getBucketLifecycle(BUCKET)).rules || [];
  const prepared = plan(first);
  if (prepared.status === 'already-configured') return prepared.status;
  if (!apply) return 'ready-to-upgrade';
  const second = (await storage.getBucketLifecycle(BUCKET)).rules || [];
  if (JSON.stringify(second) !== JSON.stringify(first))
    throw new Error('Lifecycle rules changed during preflight');
  await storage.putBucketLifecycle(BUCKET,structuredClone(prepared.rules));
  const after = (await storage.getBucketLifecycle(BUCKET)).rules || [];
  if (plan(after).status !== 'already-configured')
    throw new Error('Session attachment lifecycle rule was not confirmed');
  return 'updated';
}

async function main(args = process.argv.slice(2), env = process.env) {
  const apply = args.includes('--apply');
  const confirmation = `--confirm-bucket=${BUCKET}`;
  if (args.length !== (apply ? 2 : 1) || !args.includes(confirmation) ||
      args.some(arg => arg !== confirmation && arg !== '--apply'))
    throw new Error('Use [--apply] --confirm-bucket=<reviewed-private-bucket>');
  if (apply && process.getuid?.() !== 0)
    throw new Error('Apply must run as the authorized ECS administrator');
  if (!env.ALIBABA_CLOUD_ACCESS_KEY_ID || !env.ALIBABA_CLOUD_ACCESS_KEY_SECRET)
    throw new Error('Temporary administrator OSS credentials are missing');
  const OSS = require('ali-oss');
  const storage = new OSS({ accessKeyId:env.ALIBABA_CLOUD_ACCESS_KEY_ID,
    accessKeySecret:env.ALIBABA_CLOUD_ACCESS_KEY_SECRET,
    bucket:BUCKET,region:REGION,authorizationV4:true,secure:true });
  process.stdout.write(`${await upgrade(storage,apply)}\n`);
}

if (require.main === module) {
  main().catch(error => {
    process.stderr.write(`Session lifecycle upgrade failed: ${error.message}\n`);
    process.exitCode = 1;
  });
}

module.exports = { plan,upgrade,main,BUCKET,PREFIX };
