'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const { parseArgs,accountPrefix,inventory,cleanup,recoverDisposable,
  completeWithRecovery,deletionEvidence } =
  require('./cloud-drive-security-acceptance');

const BUCKET = 'maichat-private-cloud-drive-gz-20261025-f346b709';
const ACCOUNT = 'test_1234567890abcdef';
const KEY = `cloud-drive/${ACCOUNT}/2026-10-25/` +
  '123e4567-e89b-42d3-a456-426614174000.mp4';

test('real OSS security run needs exact dedicated bucket and root-only credential path', () => {
  const args = [`--execute=${BUCKET}`,
    '--env-file=/etc/maichat/cloud-drive.env'];
  assert.equal(parseArgs(args).execute,BUCKET);
  assert.throws(() => parseArgs([`--execute=maichat-ai-video-resource`,
    args[1]]),/dedicated bucket/);
  assert.throws(() => parseArgs([args[0],
    '--env-file=/etc/maichat/media.env']),/credential file/);
  assert.throws(() => parseArgs([...args,'--recover=../../other.json']),
    /credential file/);
  assert.equal(parseArgs([...args,
    '--recover=security-123e4567-e89b-42d3-a456-426614174000.json']).recover,
  'security-123e4567-e89b-42d3-a456-426614174000.json');
  assert.equal(accountPrefix(ACCOUNT),`cloud-drive/${ACCOUNT}/`);
  assert.throws(() => accountPrefix('real_user'),/Invalid disposable/);
});

test('cleanup touches only validated disposable account objects and Multipart uploads',
async () => {
  const actions = [];
  let objects = [{ name:KEY }];
  let uploads = [{ name:KEY,uploadId:'test-upload-id' }];
  const storage = {
    list:async options => {
      actions.push(['list',options.prefix]);
      return { objects,isTruncated:'false' };
    },
    listUploads:async options => {
      actions.push(['listUploads',options.prefix]);
      return { uploads,isTruncated:'false' };
    },
    abortMultipartUpload:async (key,id) => {
      actions.push(['abort',key,id]); uploads = [];
    },
    delete:async key => { actions.push(['delete',key]); objects = []; }
  };
  await cleanup(storage,ACCOUNT);
  assert.deepEqual(actions.slice(2,4),[
    ['abort',KEY,'test-upload-id'],['delete',KEY]]);
  assert.equal((await inventory(storage,ACCOUNT)).objects.length,0);
  objects = [{ name:'cloud-drive/other_account/2026-10-25/' +
    '123e4567-e89b-42d3-a456-426614174000.mp4' }];
  await assert.rejects(cleanup(storage,ACCOUNT),/Unexpected data/);
  assert.equal(actions.filter(action => action[0] === 'delete').length,1);
});

test('account deletion evidence is confined to an unprovisioned temporary account', () => {
  const evidence = deletionEvidence();
  assert.equal(evidence.operator,'cloud_acceptance');
  for (const key of ['tencent_im_deleted','session_sync_deleted',
    'oss_and_ark_assets_reviewed','shared_content_reviewed']) {
    assert.equal(evidence[key],true);
    assert.match(evidence.records[key],/disposable-local-test-account/);
  }
});

test('unknown Complete result leaves the durable recovery hold in place', async () => {
  const phases = [];
  const prepare = async () => phases.push('journal-in-flight');
  const settled = async () => phases.push('journal-settled');
  await assert.rejects(completeWithRecovery(prepare,settled,
    async () => { phases.push('complete'); throw new Error('connection reset'); }),
  /connection reset/);
  assert.deepEqual(phases,['journal-in-flight','complete']);
  phases.length = 0;
  await assert.rejects(completeWithRecovery(prepare,settled,
    async () => { throw Object.assign(new Error('refused'),{ status:412 }); },
    error => error.status === 412),/refused/);
  assert.deepEqual(phases,['journal-in-flight','journal-settled']);
  phases.length = 0;
  await completeWithRecovery(prepare,settled,async () => 'completed');
  assert.deepEqual(phases,['journal-in-flight','journal-settled']);
});

test('late Complete cannot clear journal before 24-hour recheck', async () => {
  const holdUntil = 1_000_000 + 24*60*60*1000;
  const record = { account_id:ACCOUNT,complete_hold_until_ms:holdUntil };
  let objects = [];
  const actions = [];
  const storage = {
    list:async () => ({ objects,isTruncated:false }),
    listUploads:async () => ({ uploads:[],isTruncated:false }),
    delete:async key => { actions.push(['delete',key]); objects = []; }
  };
  const finalize = async () => actions.push(['clear-journal']);
  assert.equal(await recoverDisposable(storage,record,finalize,
    () => holdUntil - 1),false);
  assert.deepEqual(actions,[]);
  // OSS may finish a Complete after the first cleanup found an empty prefix.
  objects = [{ name:KEY }];
  assert.equal(await recoverDisposable(storage,record,finalize,
    () => holdUntil),true);
  assert.deepEqual(actions,[['delete',KEY],['clear-journal']]);
});
