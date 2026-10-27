'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const { plan, upgrade, BUCKET, PREFIX } =
  require('./upgrade-session-attachment-lifecycle');

function fixture() {
  const state = { rules:[{ id:'cloud-drive-abort', prefix:'cloud-drive/',
    status:'Enabled', abortMultipartUpload:{ days:'8' } }], writes:0 };
  const storage = {
    async getBucketLifecycle(bucket) {
      assert.equal(bucket,BUCKET);
      return { rules:structuredClone(state.rules) };
    },
    async putBucketLifecycle(bucket,rules) {
      assert.equal(bucket,BUCKET);
      state.rules = structuredClone(rules);
      state.writes += 1;
    }
  };
  return { state,storage };
}

test('dry run preserves all rules, apply adds the isolated multipart rule once', async () => {
  const { state,storage } = fixture();
  assert.equal(await upgrade(storage), 'ready-to-upgrade');
  assert.equal(state.writes,0);
  assert.equal(await upgrade(storage,true), 'updated');
  assert.equal(state.writes,1);
  assert.equal(state.rules.length,2);
  assert.equal(state.rules[1].prefix,PREFIX);
  assert.equal(state.rules[1].abortMultipartUpload.days,8);
  assert.equal(await upgrade(storage,true), 'already-configured');
  assert.equal(state.writes,1);
});

test('unknown or overlapping lifecycle policy is not replaced', async () => {
  const { state,storage } = fixture();
  state.rules[0].abortMultipartUpload.days = '7';
  await assert.rejects(upgrade(storage,true),/not verified/);
  state.rules[0].abortMultipartUpload.days = '8';
  state.rules.push({ id:'overlap',prefix:'session-',status:'Enabled',
    abortMultipartUpload:{ days:'8' } });
  await assert.rejects(upgrade(storage,true),/overlapping/);
  assert.equal(state.writes,0);
});

test('a concurrent lifecycle change aborts before the write', async () => {
  const { state,storage } = fixture();
  const originalRead = storage.getBucketLifecycle;
  let reads = 0;
  storage.getBucketLifecycle = async bucket => {
    if (++reads === 2) state.rules.push({ id:'other',prefix:'other/',
      status:'Enabled',expiration:{ days:'30' } });
    return originalRead(bucket);
  };
  await assert.rejects(upgrade(storage,true),/changed during preflight/);
  assert.equal(state.writes,0);
});

test('conflicting Session rule aborts before the write', async () => {
  const { state,storage } = fixture();
  state.rules.push({ id:'wrong',prefix:PREFIX,status:'Enabled',
    abortMultipartUpload:{ days:'7' } });
  await assert.rejects(upgrade(storage,true),/differs/);
  assert.equal(state.writes,0);
  assert.throws(() => plan(state.rules),/differs/);
});
