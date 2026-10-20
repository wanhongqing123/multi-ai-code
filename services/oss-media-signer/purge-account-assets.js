// Operator-only cleanup for account-scoped OSS keys issued after the privacy migration.
'use strict';

const PREFIXES = ['ark-asset-inputs', 'avatars', 'kling-inputs',
  'minimax-inputs', 'seedream-inputs', 'wan-inputs'];

async function purgeAccountAssets(client, accountId, execute = false) {
  if (typeof accountId !== 'string' || !/^[a-z][a-z0-9_-]{2,31}$/.test(accountId))
    throw new Error('Invalid MaiChat account ID');
  const result = {};
  for (const root of PREFIXES) {
    const prefix = `${root}/${accountId}/`;
    let marker;
    let count = 0;
    while (true) {
      const page = await client.list({ prefix, marker, 'max-keys': 1000 });
      const keys = (page.objects || []).map(object => object.name);
      if (keys.some(key => !key.startsWith(prefix)))
        throw new Error('OSS returned an object outside the requested account prefix');
      if (!keys.length) break;
      count += keys.length;
      if (execute) await client.deleteMulti(keys, { quiet: true });
      const next = keys.at(-1);
      if (next === marker) throw new Error('OSS pagination did not advance');
      marker = next;
      if (keys.length < 1000) break;
    }
    result[root] = count;
  }
  return { account_id: accountId, execute, object_counts: result };
}

async function main(args = process.argv.slice(2), env = process.env) {
  const execute = args[0] === '--execute';
  const accountId = execute ? args[1] : args[0];
  const confirmed = execute ? args[2] : accountId;
  if (args.length !== (execute ? 3 : 1) || accountId !== confirmed ||
      !env.MAICHAT_OSS_BUCKET || !env.MAICHAT_OSS_REGION ||
      !env.ALIBABA_CLOUD_ACCESS_KEY_ID || !env.ALIBABA_CLOUD_ACCESS_KEY_SECRET)
    throw new Error('Use [--execute] <account-id> [same-account-id] with OSS credentials');
  const OSS = require('ali-oss');
  const client = new OSS({ accessKeyId: env.ALIBABA_CLOUD_ACCESS_KEY_ID,
    accessKeySecret: env.ALIBABA_CLOUD_ACCESS_KEY_SECRET,
    bucket: env.MAICHAT_OSS_BUCKET, region: env.MAICHAT_OSS_REGION,
    authorizationV4: true, secure: true });
  const result = await purgeAccountAssets(client, accountId, execute);
  process.stdout.write(`${JSON.stringify(result)}\n`);
}

if (require.main === module) {
  main().catch(error => {
    process.stderr.write(`${error.message}\n`);
    process.exitCode = 1;
  });
}

module.exports = { purgeAccountAssets, main };
