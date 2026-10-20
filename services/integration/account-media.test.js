// 文件作用：验证账号、素材两个独立进程的鉴权契约可联通。
// 使用内存账号库和假 OSS 签名，不发送短信、上传文件或调用模型。
'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const { openStore } = require('../auth/store');
const { createAuthService } = require('../auth/service');
const { createHandler: createMediaHandler } = require('../oss-media-signer/index');

test('account bearer authorizes OSS signing and logout revokes it', async () => {
  const db = openStore(':memory:');
  let sentCode = '';
  const auth = createAuthService({ db, sdkAppId: 123456789,
    sms: { async send(_phone, code) { sentCode = code; } },
    secret: 'test-only-auth-hmac-key-keep-on-server-123456',
    imSecret: 'test-only-tencent-im-key-keep-on-server-123456',
    now: () => 1_780_000_000_000 });
  const call = (method, path, body = {}, token = '') => auth.handle({
    method, path, body, ip: '192.0.2.1',
    headers: token ? { authorization: `Bearer ${token}` } : {}
  });
  try {
    const challenge = await call('POST', '/v1/auth/sms/challenge',
      { phone: '13900000001', age_confirmed_14_plus: true });
    assert.equal(challenge.statusCode, 200);
    const signedIn = await call('POST', '/v1/auth/sms/verify', {
      phone: '13900000001', challenge_id: challenge.body.challenge_id,
      code: sentCode, device_slot: 'ios_phone', age_confirmed_14_plus: true
    });
    assert.equal(signedIn.statusCode, 200);
    const token = signedIn.body.access_token;
    const media = createMediaHandler({
      env: { MAICHAT_OSS_BUCKET: 'private-test', MAICHAT_OSS_REGION: 'oss-cn-guangzhou',
        ALIBABA_CLOUD_ACCESS_KEY_ID: 'test-id',
        ALIBABA_CLOUD_ACCESS_KEY_SECRET: 'test-secret' },
      newID: () => 'account-test',
      makeClient: () => ({ signatureUrlV4: async () => 'https://example.test/signed' }),
      fetcher: async (url, options) => {
        assert.equal(url, 'http://127.0.0.1:9100/v1/media/authorize');
        const bearer = options.headers.Authorization.slice('Bearer '.length);
        const result = await call('GET', '/v1/media/authorize', {}, bearer);
        return { status: result.statusCode,
          json: async () => result.body };
      }
    });
    const event = bearer => ({ rawPath: '/sign-upload', httpMethod: 'POST',
      headers: { Authorization: `Bearer ${bearer}` },
      body: JSON.stringify({ filename: 'clip.mp4', size_bytes: 1024,
        purpose: 'ark_asset_input' }) });
    const signed = await media(event(token));
    assert.equal(signed.statusCode, 200);
    assert.match(JSON.parse(signed.body).object_key,
      new RegExp(`^ark-asset-inputs/${signedIn.body.user.id}/`));
    await call('POST', '/v1/auth/logout', {}, token);
    assert.equal((await media(event(token))).statusCode, 401);
  } finally { db.close(); }
});
