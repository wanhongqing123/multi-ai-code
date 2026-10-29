'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const { createAsrCredentialIssuer } = require('./asr-credential');

const env = { MAICHAT_ASR_APP_ID: '1234567890', MAICHAT_ASR_SECRET_ID: 'AKID12345678',
  MAICHAT_ASR_SECRET_KEY: 'test-only-server-secret-key' };
const start = 1_780_000_000_000;

function certificate(expiresAt = start / 1000 + 1800) {
  return { Credentials: { TmpSecretId: 'AKIDTemporary12345678',
    TmpSecretKey: 'test-only-temporary-secret-key', Token: 'test-only-session-token' },
  ExpiredTime: expiresAt };
}

test('uses the official STS client contract with only realtime ASR permission', async () => {
  const calls = [];
  const issue = createAsrCredentialIssuer(env, { currentTime: () => start,
    createClient: config => {
      assert.deepEqual(config.credential, { secretId: env.MAICHAT_ASR_SECRET_ID,
        secretKey: env.MAICHAT_ASR_SECRET_KEY });
      assert.equal(config.profile.httpProfile.endpoint, 'sts.tencentcloudapi.com');
      assert.equal(config.profile.httpProfile.reqTimeout, 8);
      return { async GetFederationToken(request) {
        calls.push(request);
        return { ...certificate(), RequestId: 'not-forwarded-to-app' };
      } };
    } });
  const issued = await issue();
  assert.deepEqual(issued, { app_id: env.MAICHAT_ASR_APP_ID,
    secret_id: 'AKIDTemporary12345678', secret_key: 'test-only-temporary-secret-key',
    token: 'test-only-session-token', expires_at: start / 1000 + 1800 });
  assert.equal(calls.length, 1);
  assert.equal(calls[0].Name, 'maichatasr');
  assert.equal(calls[0].DurationSeconds, 1800);
  assert.deepEqual(JSON.parse(decodeURIComponent(calls[0].Policy)), {
    version: '2.0', statement: [{ effect: 'allow',
      action: ['name/asr:RealtimeRecognition'], resource: ['*'] }]
  });
  assert.ok(!JSON.stringify(issued).includes(env.MAICHAT_ASR_SECRET_ID));
  assert.ok(!JSON.stringify(issued).includes(env.MAICHAT_ASR_SECRET_KEY));
});

test('deduplicates concurrent STS requests and refreshes 90 seconds before expiry', async () => {
  let current = start;
  let calls = 0;
  let release;
  const issue = createAsrCredentialIssuer(env, { currentTime: () => current,
    createClient: () => ({ async GetFederationToken() {
      ++calls;
      if (calls === 1) await new Promise(resolve => { release = resolve; });
      return certificate(current / 1000 + 1800);
    } }) });
  const first = issue();
  const second = issue();
  assert.equal(calls, 1);
  release();
  const issued = await first;
  assert.deepEqual(await second, issued);
  // A caller cannot mutate the cached provider response shared by other users.
  issued.secret_key = 'caller-modified-value';
  assert.equal((await issue()).secret_key, 'test-only-temporary-secret-key');
  current += 1709_000;
  assert.equal((await issue()).expires_at, start / 1000 + 1800);
  assert.equal(calls, 1);
  current += 1000;
  assert.equal((await issue()).expires_at, current / 1000 + 1800);
  assert.equal(calls, 2);
});

test('a failed refresh drops stale credentials and the next request can recover', async () => {
  let current = start;
  let calls = 0;
  const issue = createAsrCredentialIssuer(env, { currentTime: () => current,
    createClient: () => ({ async GetFederationToken() {
      ++calls;
      if (calls === 2) throw new Error('Provider failure containing private details');
      return certificate(current / 1000 + 1800);
    } }) });
  await issue();
  current += 1801_000;
  await assert.rejects(issue(), /Provider failure/);
  assert.equal((await issue()).expires_at, current / 1000 + 1800);
  assert.equal(calls, 3);
});

test('invalid, expired or permanent credentials are rejected and never cached', async () => {
  const invalid = [
    null, {}, certificate(start / 1000), certificate(start / 1000 + 90),
    certificate(start / 1000 + 3600),
    { ...certificate(), Credentials: { ...certificate().Credentials, Token: '' } },
    { ...certificate(), Credentials: { ...certificate().Credentials,
      TmpSecretId: env.MAICHAT_ASR_SECRET_ID } },
    { ...certificate(), Credentials: { ...certificate().Credentials,
      TmpSecretKey: env.MAICHAT_ASR_SECRET_KEY } }
  ];
  for (const response of invalid) {
    let calls = 0;
    const issue = createAsrCredentialIssuer(env, { currentTime: () => start,
      createClient: () => ({ async GetFederationToken() {
        ++calls;
        return calls === 1 ? response : certificate();
      } }) });
    await assert.rejects(issue(), /invalid ASR credential/);
    assert.equal((await issue()).expires_at, start / 1000 + 1800);
    assert.equal(calls, 2);
  }
});

test('unconfigured ASR stays disabled and partial configuration fails startup', () => {
  assert.equal(createAsrCredentialIssuer({}), null);
  assert.throws(() => createAsrCredentialIssuer({ ...env,
    MAICHAT_ASR_SECRET_KEY: '' }), /credentials are invalid/);
});
