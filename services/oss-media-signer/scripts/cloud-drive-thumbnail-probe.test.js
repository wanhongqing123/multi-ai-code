'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const path = require('node:path');
const { args,source,safeKey } = require('./cloud-drive-thumbnail-probe');

const base = ['--run','--bucket=maichat-private-cloud-test',
  '--region=oss-cn-guangzhou','--env-file=/tmp/root-only.env',
  '--account-id=test_account_a','--confirm-account=test_account_a',
  '--video-file=/tmp/disposable.mp4','--image-file=/tmp/disposable.png'];

test('real thumbnail probe requires an explicit exact test account and dedicated bucket', () => {
  assert.equal(args(base)['account-id'],'test_account_a');
  for (const invalid of [
    base.filter(item => item !== '--run'),
    base.map(item => item.startsWith('--confirm-account=')
      ? '--confirm-account=other_account' : item),
    base.map(item => item.startsWith('--bucket=')
      ? '--bucket=maichat-ai-video-resource' : item),
    base.map(item => item.startsWith('--account-id=')
      ? '--account-id=../../owner' : item)
  ]) assert.throws(() => args(invalid));
  assert.match(safeKey('test_account_a','image'),
    /^cloud-drive\/test_account_a\/\d{4}-\d{2}-\d{2}\/[a-f0-9-]{36}\.png$/);
});

test('probe fixture gate accepts only small regular files with matching suffix', async () => {
  const valid = path.resolve(__dirname,'../../../MaiChat/desktop/tests/fixtures/maichat-friend-qr.png');
  assert.equal((await source(valid,'image')).contentType,'image/png');
  await assert.rejects(source(valid,'video'));
});
