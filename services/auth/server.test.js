'use strict';

const assert = require('node:assert/strict');
const { createHash, randomBytes } = require('node:crypto');
const test = require('node:test');
const { createAuthService } = require('./service');
const { openStore } = require('./store');
const { createServer, start, requireDeviceSlotFromEnv,
  videoCircleFromEnv } = require('./server');

test('device slot cutover flag defaults off and rejects ambiguous values', () => {
  assert.equal(requireDeviceSlotFromEnv(undefined), false);
  assert.equal(requireDeviceSlotFromEnv(''), false);
  assert.equal(requireDeviceSlotFromEnv('0'), false);
  assert.equal(requireDeviceSlotFromEnv('1'), true);
  assert.throws(() => requireDeviceSlotFromEnv('true'), /must be 0 or 1/);
});

test('video circle signer is off by default and only calls loopback with its internal key', async () => {
  assert.equal(videoCircleFromEnv({}), null);
  assert.equal(videoCircleFromEnv({ MAICHAT_VIDEO_CIRCLE_ENABLED: '0' }), null);
  assert.throws(() => videoCircleFromEnv({ MAICHAT_VIDEO_CIRCLE_ENABLED: '1' }),
    /internal signer/);
  const calls = [];
  const secret = 'circle-test-internal-key-at-least-32-characters';
  const connector = videoCircleFromEnv({ MAICHAT_VIDEO_CIRCLE_ENABLED: '1',
    MAICHAT_VIDEO_CIRCLE_INTERNAL_KEY: secret, MAICHAT_MEDIA_PORT: '9000' },
  async (url, options) => {
    calls.push({ url, options });
    return { status: 200, json: async () => ({ exists: true, size_bytes: 12,
      content_type: 'video/mp4', read_url: 'https://private.example/short', deleted: true }) };
  });
  assert.equal((await connector.verifyUpload('author', 'video-circle/author/id.mp4', 12)).exists,
    true);
  assert.equal(await connector.signRead('video-circle/author/id.mp4'),
    'https://private.example/short');
  await connector.deleteObject('video-circle/author/id.mp4');
  assert.deepEqual(calls.map(call => call.url), [
    'http://127.0.0.1:9000/video-circle/verify',
    'http://127.0.0.1:9000/video-circle/read',
    'http://127.0.0.1:9000/video-circle/delete'
  ]);
  assert.ok(calls.every(call => call.options.headers['X-MaiChat-Video-Circle-Key'] === secret));
});

test('video circle cannot start without a protected review server', () => {
  assert.throws(() => start({ MAICHAT_VIDEO_CIRCLE_ENABLED: '1' }),
    /authenticated admin review server/);
});

test('HTTP adapter rejects oversized and invalid requests without invoking auth', async () => {
  let calls = 0;
  const server = createServer({ async handle() {
    ++calls;
    return { statusCode: 200, body: { ok: true } };
  } });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  try {
    const url = `http://127.0.0.1:${server.address().port}/v1/auth/sms/challenge`;
    const invalid = await fetch(url, { method: 'POST', body: '{' });
    assert.equal(invalid.status, 400);
    assert.equal(invalid.headers.get('cache-control'), 'no-store');
    const oversized = await fetch(url, { method: 'POST', body: 'x'.repeat(4097) });
    assert.equal(oversized.status, 413);
    assert.equal(calls, 0);
    const valid = await fetch(url, { method: 'POST', body: '{}' });
    assert.equal(valid.status, 200);
    assert.equal(calls, 1);
  } finally {
    await new Promise(resolve => server.close(resolve));
  }
});

test('HTTP ASR STS responses require a session and never permit credential caching', async () => {
  const db = openStore(':memory:');
  let smsCode = '';
  let fail = false;
  const service = createAuthService({ db,
    sms: { async send(_phone, code) { smsCode = code; } },
    secret: 'test-only-auth-hmac-key-keep-on-server-123456',
    imSecret: 'test-only-tencent-im-key-keep-on-server-123456',
    sdkAppId: 123456789, now: () => 1_780_000_000_000,
    issueAsrCredential: async () => {
      if (fail) throw new Error('private-provider-details');
      return { app_id: '1234567890', secret_id: 'TMP-fake+/=_:1234567890',
        secret_key: 'test-only-temporary-secret-key', token: 'test-only-session-token',
        expires_at: 1_780_001_800 };
    } });
  const challenge = await service.handle({ method: 'POST', path: '/v1/auth/sms/challenge',
    body: { phone: '13900000001', age_confirmed_14_plus: true }, ip: '192.0.2.1' });
  const signedIn = await service.handle({ method: 'POST', path: '/v1/auth/sms/verify',
    body: { phone: '13900000001', challenge_id: challenge.body.challenge_id,
      code: smsCode, device_slot: 'ios_phone', age_confirmed_14_plus: true } });
  assert.equal(signedIn.statusCode, 200);
  const server = createServer(service);
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  try {
    const endpoint = `http://127.0.0.1:${server.address().port}/v1/asr/credential`;
    const request = token => fetch(endpoint, { method: 'POST', body: '{}',
      headers: { 'content-type': 'application/json',
        ...(token ? { authorization: `Bearer ${token}` } : {}) } });
    const unauthenticated = await request();
    assert.equal(unauthenticated.status, 401);
    assert.equal(unauthenticated.headers.get('cache-control'), 'no-store');
    const issued = await request(signedIn.body.access_token);
    assert.equal(issued.status, 200);
    assert.equal(issued.headers.get('cache-control'), 'no-store');
    const credential = await issued.json();
    assert.equal(credential.token, 'test-only-session-token');
    assert.equal(credential.secret_id, 'TMP-fake+/=_:1234567890');
    fail = true;
    const unavailable = await request(signedIn.body.access_token);
    assert.equal(unavailable.status, 503);
    assert.equal(unavailable.headers.get('cache-control'), 'no-store');
    assert.deepEqual(await unavailable.json(), { error: 'Speech recognition is unavailable' });
    // Retired URL signing is deliberately unavailable; older clients must upgrade.
    const retired = await fetch(`http://127.0.0.1:${server.address().port}` +
      '/v1/asr/realtime-url', { method: 'POST', body: '{}',
      headers: { authorization: `Bearer ${signedIn.body.access_token}` } });
    assert.equal(retired.status, 404);
    assert.equal(retired.headers.get('cache-control'), 'no-store');
  } finally {
    await new Promise(resolve => server.close(resolve));
    db.close();
  }
});

test('HTTP desktop request returns a renderable QR PNG and matching URI', async () => {
  const db = openStore(':memory:');
  const service = createAuthService({
    db, sms: { async send() {} },
    secret: 'test-only-auth-hmac-key-keep-on-server-123456',
    imSecret: 'test-only-tencent-im-key-keep-on-server-123456',
    sdkAppId: 123456789
  });
  const server = createServer(service);
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  try {
    const proof = randomBytes(32);
    const response = await fetch(`http://127.0.0.1:${server.address().port}` +
      '/v1/auth/desktop/request', { method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ platform: 'mac', code_challenge:
        createHash('sha256').update(proof).digest('base64url') }) });
    assert.equal(response.status, 200);
    const request = await response.json();
    assert.equal(new URL(request.qr_uri).searchParams.get('request_id'),
      request.request_id);
    assert.equal(Buffer.from(request.qr_png_base64, 'base64').subarray(0, 8)
      .toString('hex'), '89504e470d0a1a0a');
    assert.equal(response.headers.get('cache-control'), 'no-store');
  } finally {
    await new Promise(resolve => server.close(resolve));
    db.close();
  }
});

test('HTTP social login binds a verified identity through the phone challenge', async () => {
  const db = openStore(':memory:');
  let smsCode = '';
  const service = createAuthService({
    db, sms: { async send(_phone, code) { smsCode = code; } },
    secret: 'test-only-auth-hmac-key-keep-on-server-123456',
    imSecret: 'test-only-tencent-im-key-keep-on-server-123456',
    sdkAppId: 123456789,
    socialVerifiers: { wechat: async code => code === 'valid-code' ?
      { appId: 'wx-test', subject: 'official-openid' } : null }
  });
  const server = createServer(service);
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  try {
    const base = `http://127.0.0.1:${server.address().port}`;
    const post = async (path, body) => {
      const response = await fetch(`${base}${path}`, { method: 'POST',
        headers: { 'content-type': 'application/json' }, body: JSON.stringify(body) });
      return { status: response.status, body: await response.json() };
    };
    const social = await post('/v1/auth/social/wechat/login', {
      code: 'valid-code', device_slot: 'ios_phone', age_confirmed_14_plus: true
    });
    assert.equal(social.status, 202);
    const challenge = await post('/v1/auth/sms/challenge', {
      phone: '13900000001', purpose: 'social_bind', age_confirmed_14_plus: true
    });
    assert.equal(challenge.status, 200);
    const bound = await post('/v1/auth/social/bind', {
      binding_token: social.body.binding_token, phone: '13900000001',
      challenge_id: challenge.body.challenge_id, code: smsCode,
      age_confirmed_14_plus: true
    });
    assert.equal(bound.status, 200);
    assert.equal(bound.body.device_slot, 'ios_phone');
    const me = await fetch(`${base}/v1/auth/me`, {
      headers: { authorization: `Bearer ${bound.body.access_token}` }
    });
    assert.equal(me.status, 200);
    assert.equal((await me.json()).user.im_user_id, bound.body.user.im_user_id);
  } finally {
    await new Promise(resolve => server.close(resolve));
    db.close();
  }
});
