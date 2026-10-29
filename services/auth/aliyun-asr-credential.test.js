'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const { createAliyunAsrCredentialIssuer } = require('./aliyun-asr-credential');

const env = { MAICHAT_NLS_APP_KEY: 'testOnlyNlsAppKey',
  MAICHAT_NLS_ACCESS_KEY_ID: 'test-only-nls-access-key-id',
  MAICHAT_NLS_ACCESS_KEY_SECRET: 'test-only-nls-access-key-secret' };
const start = 1_780_000_000_000;
const reply = (expiresAt = start / 1000 + 86400) => ({
  Token: { Id: 'test-only-nls-temporary-token', ExpireTime: expiresAt }
});

test('Aliyun ASR uses the official RPC contract and preserves provider expiration', async () => {
  const calls = [];
  const issue = createAliyunAsrCredentialIssuer(env, { currentTime: () => start,
    createClient: config => {
      assert.deepEqual(config, { accessKeyId: env.MAICHAT_NLS_ACCESS_KEY_ID,
        accessKeySecret: env.MAICHAT_NLS_ACCESS_KEY_SECRET,
        endpoint: 'https://nls-meta.cn-shanghai.aliyuncs.com', apiVersion: '2019-02-28' });
      return { async request(...parameters) { calls.push(parameters); return reply(); } };
    } });
  const credential = await issue();
  assert.deepEqual(credential, { app_key: env.MAICHAT_NLS_APP_KEY,
    token: 'test-only-nls-temporary-token', expires_at: start / 1000 + 86400 });
  assert.deepEqual(calls, [['CreateToken', {}, { method: 'POST', timeout: 8000 }]]);
  assert.ok(!JSON.stringify(credential).includes(env.MAICHAT_NLS_ACCESS_KEY_ID));
  assert.ok(!JSON.stringify(credential).includes(env.MAICHAT_NLS_ACCESS_KEY_SECRET));
});

test('Aliyun ASR merges concurrent requests and refreshes at the 90-second boundary', async () => {
  let current = start;
  let calls = 0;
  let release;
  const issue = createAliyunAsrCredentialIssuer(env, { currentTime: () => current,
    createClient: () => ({ async request() {
      ++calls;
      if (calls === 1) await new Promise(resolve => { release = resolve; });
      return reply(Math.floor(current / 1000) + 3600);
    } }) });
  const first = issue();
  const concurrent = issue();
  assert.equal(calls, 1);
  release();
  const credential = await first;
  assert.deepEqual(await concurrent, credential);
  credential.token = 'caller-modified-value';
  assert.equal((await issue()).token, 'test-only-nls-temporary-token');
  current += 3509_000;
  assert.equal((await issue()).expires_at, start / 1000 + 3600);
  assert.equal(calls, 1);
  current += 1000;
  assert.equal((await issue()).expires_at, current / 1000 + 3600);
  assert.equal(calls, 2);
});

test('Aliyun ASR failed refresh never falls back to stale tokens and can recover', async () => {
  let current = start;
  let calls = 0;
  const issue = createAliyunAsrCredentialIssuer(env, { currentTime: () => current,
    createClient: () => ({ async request() {
      ++calls;
      if (calls === 2) throw new Error('private-provider-failure');
      return reply(Math.floor(current / 1000) + 3600);
    } }) });
  await issue();
  current += 3601_000;
  await assert.rejects(issue(), /private-provider-failure/);
  assert.equal((await issue()).expires_at, current / 1000 + 3600);
  assert.equal(calls, 3);
});

test('Aliyun ASR rejects invalid, stale and accidental permanent values without caching them', async () => {
  const invalid = [null, {}, { Token: {} }, reply(start / 1000),
    reply(start / 1000 + 90), reply(String(start / 1000 + 3600)),
    ...['', 'token with whitespace', 'token\nwith-control', 'x'.repeat(8193),
      env.MAICHAT_NLS_ACCESS_KEY_ID, env.MAICHAT_NLS_ACCESS_KEY_SECRET]
      .map(token => ({ Token: { Id: token, ExpireTime: start / 1000 + 3600 } }))];
  for (const response of invalid) {
    let calls = 0;
    const issue = createAliyunAsrCredentialIssuer(env, { currentTime: () => start,
      createClient: () => ({ async request() { return ++calls === 1 ? response : reply(); } }) });
    await assert.rejects(issue(), /invalid speech token/);
    assert.equal((await issue()).token, 'test-only-nls-temporary-token');
    assert.equal(calls, 2);
  }
});

test('Aliyun ASR missing configuration disables only NLS and never falls back to SMS keys', () => {
  const configurations = [{}, { MAICHAT_NLS_APP_KEY: env.MAICHAT_NLS_APP_KEY },
    { ...env, MAICHAT_NLS_ACCESS_KEY_SECRET: '' },
    { ...env, MAICHAT_NLS_APP_KEY: 'invalid key with spaces' },
    { MAICHAT_NLS_APP_KEY: env.MAICHAT_NLS_APP_KEY,
      MAICHAT_SMS_ACCESS_KEY_ID: env.MAICHAT_NLS_ACCESS_KEY_ID,
      MAICHAT_SMS_ACCESS_KEY_SECRET: env.MAICHAT_NLS_ACCESS_KEY_SECRET }];
  for (const configuration of configurations)
    assert.equal(createAliyunAsrCredentialIssuer(configuration, {
      createClient() { assert.fail('Disabled NLS must not construct a client'); }
    }), null);
});
