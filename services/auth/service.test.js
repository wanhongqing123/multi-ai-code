'use strict';

const assert = require('node:assert/strict');
const { createHash, createHmac, randomBytes } = require('node:crypto');
const { mkdtempSync, rmSync } = require('node:fs');
const { tmpdir } = require('node:os');
const { join } = require('node:path');
const { inflateSync } = require('node:zlib');
const test = require('node:test');
const { generatePngQrCode } = require('@juit/qrcode');
const Database = require('better-sqlite3');
const { openStore } = require('./store');
const { createAuthService } = require('./service');
const { bindLegacy } = require('./bind-legacy');

function fixture(extra = {}) {
  const db = openStore(':memory:');
  let current = 1_780_000_000_000;
  const messages = [];
  const service = createAuthService({
    db,
    sms: { async send(phone, code) { messages.push({ phone, code }); } },
    secret: 'test-only-auth-hmac-key-keep-on-server-123456',
    imSecret: 'test-only-tencent-im-key-keep-on-server-123456',
    sdkAppId: 123456789,
    now: () => current,
    ...extra
  });
  const rawCall = (method, path, body = {}, token = '') => service.handle({
    method, path, body, ip: '192.0.2.1',
    headers: token ? { authorization: `Bearer ${token}` } : {}
  });
  const ageCheckedPaths = new Set([
    '/v1/auth/sms/challenge', '/v1/auth/sms/verify',
    '/v1/auth/password/login', '/v1/auth/password/reset',
    '/v1/auth/social/wechat/login', '/v1/auth/social/qq/login',
    '/v1/auth/social/bind'
  ]);
  const call = (method, path, body = {}, token = '') => rawCall(
    method, path,
    method === 'POST' && ageCheckedPaths.has(path)
      ? { ...body, age_confirmed_14_plus: true } : body,
    token
  );
  return { db, messages, call, rawCall, advance(milliseconds) { current += milliseconds; } };
}

function desktopProof() {
  const verifier = randomBytes(32).toString('base64url');
  return { verifier, code_challenge: createHash('sha256')
    .update(Buffer.from(verifier, 'base64url')).digest('base64url') };
}

async function signInWithSms(setup, phone = '13900000001', device =
  { device_slot: 'ios_phone' }) {
  const challenge = await setup.call('POST', '/v1/auth/sms/challenge', { phone });
  assert.equal(challenge.statusCode, 200);
  return setup.call('POST', '/v1/auth/sms/verify', {
    phone, challenge_id: challenge.body.challenge_id,
    code: setup.messages.at(-1).code, ...device
  });
}

async function requestDesktop(setup, platform) {
  const proof = desktopProof();
  const request = await setup.call('POST', '/v1/auth/desktop/request', {
    platform, code_challenge: proof.code_challenge
  });
  assert.equal(request.statusCode, 200);
  return { proof, request };
}

test('auth/me identifies the active device login across token refresh', async () => {
  const setup = fixture();
  const first = await signInWithSms(setup);
  const firstMe = await setup.call('GET', '/v1/auth/me', {}, first.body.access_token);
  assert.equal(firstMe.statusCode, 200);
  assert.match(firstMe.body.login_session_id,
    /^[0-9a-f]{8}-[0-9a-f-]{27,}$/);
  const refreshed = await setup.call('POST', '/v1/auth/refresh', {
    refresh_token: first.body.refresh_token
  });
  assert.equal(refreshed.statusCode, 200);
  const refreshedMe = await setup.call('GET', '/v1/auth/me', {},
    refreshed.body.access_token);
  assert.equal(refreshedMe.body.login_session_id, firstMe.body.login_session_id);
  setup.advance(60_001);
  const second = await signInWithSms(setup);
  const secondMe = await setup.call('GET', '/v1/auth/me', {}, second.body.access_token);
  assert.notEqual(secondMe.body.login_session_id, firstMe.body.login_session_id);
  assert.equal((await setup.call('GET', '/v1/auth/me', {},
    refreshed.body.access_token)).statusCode, 401);
  setup.db.close();
});

test('every valid phone can receive a real SMS challenge without an allowlist', async () => {
  const setup = fixture();
  const path = '/v1/auth/sms/challenge';
  const otherPhone = '13900000002';
  const challenge = await setup.call('POST', path, { phone: otherPhone });
  assert.equal(challenge.statusCode, 200);
  assert.equal(setup.messages.length, 1);
  assert.equal(setup.messages[0].phone, otherPhone);
  assert.equal((await setup.call('POST', '/v1/auth/sms/verify', {
    phone: otherPhone, challenge_id: challenge.body.challenge_id,
    code: setup.messages[0].code
  })).statusCode, 200);
  assert.equal((await setup.call('POST', path, {
    phone: '12345678901'
  })).statusCode, 400);
  assert.equal(setup.messages.length, 1);
  setup.db.close();
});

test('phone verification never sends SMS or logs in without the age confirmation', async () => {
  const setup = fixture();
  assert.equal((await setup.rawCall('POST', '/v1/auth/sms/challenge',
    { phone: '13900000001' })).statusCode, 403);
  assert.equal(setup.messages.length, 0);
  assert.equal((await setup.rawCall('POST', '/v1/auth/password/login',
    { phone: '13900000001', password: 'any password' })).statusCode, 403);
  const challenge = await setup.call('POST', '/v1/auth/sms/challenge',
    { phone: '13900000001' });
  assert.equal(challenge.statusCode, 200);
  assert.equal((await setup.rawCall('POST', '/v1/auth/sms/verify', {
    phone: '13900000001', challenge_id: challenge.body.challenge_id,
    code: setup.messages[0].code
  })).statusCode, 403);
  assert.equal(setup.db.prepare('SELECT COUNT(*) AS count FROM users').get().count, 0);
  setup.db.close();
});

test('signed-in accounts receive short-lived Seedance credentials', async () => {
  let issued = 0;
  const setup = fixture({ issueArkCredential: async () => ({
    api_key: `temporary-key-for-test-${++issued}`,
    expires_at: 1_780_000_000 + issued * 300
  }) });
  const path = '/v1/model/seedance-credential';
  assert.equal((await setup.call('GET', path)).statusCode, 401);
  const challenge = await setup.call('POST', '/v1/auth/sms/challenge',
    { phone: '13900000001' });
  const login = await setup.call('POST', '/v1/auth/sms/verify', {
    phone: '13900000001', challenge_id: challenge.body.challenge_id,
    code: setup.messages[0].code
  });
  const [first, concurrent] = await Promise.all([
    setup.call('GET', path, {}, login.body.access_token),
    setup.call('GET', path, {}, login.body.access_token)
  ]);
  assert.equal(first.statusCode, 200);
  assert.equal(concurrent.body.api_key, first.body.api_key);
  assert.equal(first.body.expires_at, 1_780_000_300);
  assert.equal((await setup.call('GET', path, {}, login.body.access_token)).body.api_key,
    first.body.api_key);
  assert.equal(issued, 1);
  setup.advance(220_000);
  assert.equal((await setup.call('GET', path, {}, login.body.access_token)).statusCode, 200);
  assert.equal(issued, 2);
  setup.db.close();

});

test('main chat keys require a signed-in account', async () => {
  let issued = 0;
  const setup = fixture({ issueAccountModelKeys: async () => {
    ++issued;
    return { api_keys: { glm: 'glm-model-key', deepseek: 'deepseek-model-key',
      minimax: 'minimax-model-key' } };
  } });
  const path = '/v1/model/api-keys';
  assert.equal((await setup.call('GET', path)).statusCode, 401);
  const allowed = await signInWithSms(setup);
  const response = await setup.call('GET', path, {}, allowed.body.access_token);
  assert.equal(response.statusCode, 200);
  assert.deepEqual(response.body, {
    api_keys: { glm: 'glm-model-key', deepseek: 'deepseek-model-key',
      minimax: 'minimax-model-key' }
  });
  assert.equal(issued, 1);
  setup.db.close();

});

test('media signing requires a signed-in account', async () => {
  const allowed = fixture();
  const path = '/v1/media/authorize';
  assert.equal((await allowed.call('GET', path)).statusCode, 401);
  const signedIn = await signInWithSms(allowed);
  assert.deepEqual((await allowed.call('GET', path, {}, signedIn.body.access_token)).body,
    { allowed: true });
  allowed.db.close();

});

test('only a signed-in model account receives cached short-lived Bailian credentials', async () => {
  let issued = 0;
  const setup = fixture({ issueBailianCredential: async () => ({
    api_key: `st-temporary-key-${++issued}`, expires_at: 1_780_000_000 + issued * 300,
    wan_workspace_id: 'ws-test'
  }) });
  const path = '/v1/model/bailian-credential';
  assert.equal((await setup.call('GET', path)).statusCode, 401);
  const challenge = await setup.call('POST', '/v1/auth/sms/challenge',
    { phone: '13900000001' });
  const login = await setup.call('POST', '/v1/auth/sms/verify', {
    phone: '13900000001', challenge_id: challenge.body.challenge_id,
    code: setup.messages[0].code
  });
  const [first, concurrent] = await Promise.all([
    setup.call('GET', path, {}, login.body.access_token),
    setup.call('GET', path, {}, login.body.access_token)
  ]);
  assert.equal(first.statusCode, 200);
  assert.equal(first.body.api_key, concurrent.body.api_key);
  assert.equal(first.body.wan_workspace_id, 'ws-test');
  assert.equal(issued, 1);
  setup.advance(220_000);
  assert.equal((await setup.call('GET', path, {}, login.body.access_token)).statusCode, 200);
  assert.equal(issued, 2);
  setup.db.close();
});

test('a valid unlisted account can use all model and media credential routes', async () => {
  const setup = fixture({
    issueAccountModelKeys: async () => ({ api_keys: {
      glm: 'glm-key', deepseek: 'deepseek-key', minimax: '' } }),
    issueArkCredential: async () => ({
      api_key: 'temporary-seedance-key', expires_at: 1_780_000_300 }),
    issueBailianCredential: async () => ({
      api_key: 'st-temporary-bailian-key', expires_at: 1_780_000_300,
      wan_workspace_id: 'ws-test' })
  });
  const token = (await signInWithSms(setup, '13900000002')).body.access_token;
  for (const path of ['/v1/model/api-keys', '/v1/model/seedance-credential',
    '/v1/model/bailian-credential', '/v1/media/authorize']) {
    assert.equal((await setup.call('GET', path)).statusCode, 401, path);
    assert.equal((await setup.call('GET', path, {}, token)).statusCode, 200, path);
  }
  setup.db.close();
});

test('friend QR requires login and contains no account credential', async () => {
  const setup = fixture();
  const path = '/v1/friend/my-qr';
  assert.equal((await setup.call('GET', path)).statusCode, 401);
  const signedIn = await signInWithSms(setup, '13900000002');
  const response = await setup.call('GET', path, {}, signedIn.body.access_token);
  assert.equal(response.statusCode, 200);
  assert.equal(response.body.user_id, signedIn.body.user.id);
  assert.equal(Buffer.from(response.body.qr_png_base64, 'base64')
    .subarray(0, 8).toString('hex'), '89504e470d0a1a0a');
  assert.ok(!JSON.stringify(response.body).includes(signedIn.body.access_token));
  setup.db.close();
});

test('phone lookup resolves one registered account to its IM identity', async () => {
  const setup = fixture();
  const path = '/v1/friend/resolve';
  const alice = await signInWithSms(setup, '13900000001');
  const bob = await signInWithSms(setup, '13900000002');
  assert.equal((await setup.call('POST', path, { phone: '13900000002' })).statusCode, 401);
  const resolved = await setup.call('POST', path, { phone: '+86 13900000002' },
    alice.body.access_token);
  assert.equal(resolved.statusCode, 200);
  assert.deepEqual(resolved.body, { user_id: bob.body.user.im_user_id });
  assert.equal((await setup.call('POST', path, { phone: '13900000001' },
    alice.body.access_token)).statusCode, 400);
  assert.equal((await setup.call('POST', path, { phone: '13900000003' },
    alice.body.access_token)).statusCode, 404);
  for (let index = 0; index < 18; index++)
    assert.equal((await setup.call('POST', path, { phone: '13900000003' },
      alice.body.access_token)).statusCode, 404);
  assert.equal((await setup.call('POST', path, { phone: '13900000003' },
    alice.body.access_token)).statusCode, 429);
  setup.db.close();
});

test('ASR URL issuance requires a session and limits each account', async () => {
  let serial = 0;
  const setup = fixture({ issueAsrRealtimeUrl: () => ({
    voice_id: `00000000-0000-4000-8000-${String(++serial).padStart(12, '0')}`,
    url: 'wss://asr.cloud.tencent.com/asr/v2/1234567890?signed=test',
    expires_at: 1_780_000_300
  }) });
  const path = '/v1/asr/realtime-url';
  assert.equal((await setup.call('POST', path)).statusCode, 401);
  const token = (await signInWithSms(setup, '13900000002')).body.access_token;
  assert.equal((await setup.call('POST', path, { engine_model_type: 'paid' }, token))
    .statusCode, 400);
  for (let index = 0; index < 10; index++) {
    const response = await setup.call('POST', path, {}, token);
    assert.equal(response.statusCode, 200);
    assert.equal(response.body.expires_at, 1_780_000_300);
  }
  assert.equal((await setup.call('POST', path, {}, token)).statusCode, 429);
  setup.advance(60_001);
  assert.equal((await setup.call('POST', path, {}, token)).statusCode, 200);
  setup.db.close();

  const unavailable = fixture();
  const other = (await signInWithSms(unavailable)).body.access_token;
  assert.equal((await unavailable.call('POST', path, {}, other)).statusCode, 503);
  unavailable.db.close();
});

test('one SMS login creates a stable account and a server-signed IM identity', async () => {
  const setup = fixture();
  const sent = await setup.call('POST', '/v1/auth/sms/challenge',
    { phone: '13900000001' });
  assert.equal(sent.statusCode, 200);
  assert.equal(setup.messages.length, 1);
  assert.equal(setup.messages[0].phone, '13900000001');
  assert.match(setup.messages[0].code, /^[0-9]{6}$/);
  const login = await setup.call('POST', '/v1/auth/sms/verify', {
    phone: '13900000001', challenge_id: sent.body.challenge_id,
    code: setup.messages[0].code
  });
  assert.equal(login.statusCode, 200);
  assert.match(login.body.access_token, /^[A-Za-z0-9_-]{43}$/);
  assert.match(login.body.user.im_user_id, /^u[0-9a-f]{24}$/);
  const replay = await setup.call('POST', '/v1/auth/sms/verify', {
    phone: '13900000001', challenge_id: sent.body.challenge_id,
    code: setup.messages[0].code
  });
  assert.equal(replay.statusCode, 400);
  const im = await setup.call('GET', '/v1/im/credential', {}, login.body.access_token);
  assert.equal(im.statusCode, 200);
  assert.equal(im.body.user_id, login.body.user.im_user_id);
  const encoded = im.body.user_sig.replace(/\*/g, '+').replace(/-/g, '/').replace(/_/g, '=');
  const payload = JSON.parse(inflateSync(Buffer.from(encoded, 'base64')).toString('utf8'));
  assert.equal(payload['TLS.identifier'], login.body.user.im_user_id);
  const signed = `TLS.identifier:${payload['TLS.identifier']}\n` +
    `TLS.sdkappid:${payload['TLS.sdkappid']}\nTLS.time:${payload['TLS.time']}\n` +
    `TLS.expire:${payload['TLS.expire']}\n`;
  assert.equal(payload['TLS.sig'], createHmac('sha256',
    'test-only-tencent-im-key-keep-on-server-123456').update(signed).digest('base64'));
  assert.equal((await setup.call('GET', '/v1/auth/me')).statusCode, 401);
  setup.db.close();
});

test('password login, refresh rotation, and logout preserve account identity', async () => {
  const setup = fixture();
  const sent = await setup.call('POST', '/v1/auth/sms/challenge', { phone: '13900000001' });
  const initial = await setup.call('POST', '/v1/auth/sms/verify', {
    phone: '13900000001', challenge_id: sent.body.challenge_id,
    code: setup.messages[0].code
  });
  const password = 'a long passphrase for this account';
  assert.equal((await setup.call('POST', '/v1/auth/password/set',
    { new_password: password }, initial.body.access_token)).statusCode, 200);
  const bad = await setup.call('POST', '/v1/auth/password/login',
    { phone: '13900000001', password: 'wrong password' });
  assert.equal(bad.statusCode, 401);
  const logged = await setup.call('POST', '/v1/auth/password/login',
    { phone: '13900000001', password, device_slot: 'ios_phone' });
  assert.equal(logged.statusCode, 200);
  assert.equal(logged.body.device_slot, 'ios_phone');
  assert.equal(logged.body.user.id, initial.body.user.id);
  const rotated = await setup.call('POST', '/v1/auth/refresh',
    { refresh_token: logged.body.refresh_token });
  assert.equal(rotated.statusCode, 200);
  assert.equal(rotated.body.device_slot, 'ios_phone');
  assert.notEqual(rotated.body.refresh_token, logged.body.refresh_token);
  assert.equal((await setup.call('GET', '/v1/auth/me', {},
    logged.body.access_token)).statusCode, 401);
  const replacement = await setup.call('POST', '/v1/auth/password/login',
    { phone: '13900000001', password, device_slot: 'ios_phone' });
  assert.equal(replacement.statusCode, 200);
  assert.equal((await setup.call('GET', '/v1/auth/me', {},
    rotated.body.access_token)).body.code, 'SESSION_REPLACED');
  assert.equal((await setup.call('POST', '/v1/auth/refresh', {
    refresh_token: rotated.body.refresh_token
  })).body.code, 'SESSION_REPLACED');
  assert.equal((await setup.call('POST', '/v1/auth/logout', {},
    replacement.body.access_token)).statusCode, 200);
  assert.equal((await setup.call('GET', '/v1/auth/me', {},
    replacement.body.access_token)).statusCode, 401);
  setup.db.close();
});

test('resend supersedes old codes and cooldown blocks duplicate SMS', async () => {
  const setup = fixture();
  const first = await setup.call('POST', '/v1/auth/sms/challenge', { phone: '13900000001' });
  assert.equal((await setup.call('POST', '/v1/auth/sms/challenge',
    { phone: '13900000001' })).statusCode, 429);
  assert.equal(setup.messages.length, 1);
  setup.advance(61_000);
  const second = await setup.call('POST', '/v1/auth/sms/challenge', { phone: '13900000001' });
  assert.equal(setup.messages.length, 2);
  assert.equal((await setup.call('POST', '/v1/auth/sms/verify', {
    phone: '13900000001', challenge_id: first.body.challenge_id,
    code: setup.messages[0].code
  })).statusCode, 400);
  assert.equal((await setup.call('POST', '/v1/auth/sms/verify', {
    phone: '13900000001', challenge_id: second.body.challenge_id,
    code: setup.messages[1].code
  })).statusCode, 200);
  setup.db.close();
});

test('password reset revokes old sessions', async () => {
  const setup = fixture();
  const sent = await setup.call('POST', '/v1/auth/sms/challenge', { phone: '13900000001' });
  const login = await setup.call('POST', '/v1/auth/sms/verify', {
    phone: '13900000001', challenge_id: sent.body.challenge_id,
    code: setup.messages[0].code
  });
  setup.advance(61_000);
  const reset = await setup.call('POST', '/v1/auth/sms/challenge',
    { phone: '13900000001', purpose: 'reset' });
  const changed = await setup.call('POST', '/v1/auth/password/reset', {
    phone: '13900000001', challenge_id: reset.body.challenge_id,
    code: setup.messages[1].code, new_password: 'another long passphrase for account',
    device_slot: 'android_phone'
  });
  assert.equal(changed.statusCode, 200);
  assert.equal(changed.body.device_slot, 'android_phone');
  assert.equal((await setup.call('GET', '/v1/auth/me', {}, login.body.access_token)).statusCode, 401);
  assert.equal((await setup.call('GET', '/v1/auth/me', {}, changed.body.access_token)).statusCode, 200);
  setup.db.close();
});

test('five incorrect codes exhaust a challenge without creating an account', async () => {
  const setup = fixture();
  const sent = await setup.call('POST', '/v1/auth/sms/challenge', { phone: '13900000001' });
  const correct = setup.messages[0].code;
  const wrong = correct === '000000' ? '000001' : '000000';
  for (let attempt = 0; attempt < 5; ++attempt) {
    const response = await setup.call('POST', '/v1/auth/sms/verify', {
      phone: '13900000001', challenge_id: sent.body.challenge_id, code: wrong
    });
    assert.equal(response.statusCode, 400);
  }
  assert.equal((await setup.call('POST', '/v1/auth/sms/verify', {
    phone: '13900000001', challenge_id: sent.body.challenge_id, code: correct
  })).statusCode, 400);
  assert.equal(setup.db.prepare('SELECT COUNT(*) AS count FROM users').get().count, 0);
  setup.db.close();
});

test('replaying an old refresh token revokes the rotated session', async () => {
  const setup = fixture();
  const sent = await setup.call('POST', '/v1/auth/sms/challenge', { phone: '13900000001' });
  const login = await setup.call('POST', '/v1/auth/sms/verify', {
    phone: '13900000001', challenge_id: sent.body.challenge_id,
    code: setup.messages[0].code
  });
  const rotated = await setup.call('POST', '/v1/auth/refresh',
    { refresh_token: login.body.refresh_token });
  assert.equal(rotated.statusCode, 200);
  assert.equal((await setup.call('POST', '/v1/auth/refresh',
    { refresh_token: login.body.refresh_token })).statusCode, 401);
  assert.equal((await setup.call('GET', '/v1/auth/me', {},
    rotated.body.access_token)).statusCode, 401);
  setup.db.close();
});

test('an administrator-approved legacy IM ID is reused at first phone login', async () => {
  const setup = fixture();
  bindLegacy(setup.db, '13900000001', 'oldphoneuser');
  const sent = await setup.call('POST', '/v1/auth/sms/challenge', { phone: '13900000001' });
  const login = await setup.call('POST', '/v1/auth/sms/verify', {
    phone: '13900000001', challenge_id: sent.body.challenge_id,
    code: setup.messages[0].code
  });
  assert.equal(login.statusCode, 200);
  assert.equal(login.body.user.im_user_id, 'oldphoneuser');
  assert.throws(() => bindLegacy(setup.db, '13900000001', 'anotheruser'));
  setup.db.close();
});

test('expired verification records and sessions are purged after the safety window', async () => {
  const setup = fixture();
  const sent = await setup.call('POST', '/v1/auth/sms/challenge', { phone: '13900000001' });
  const login = await setup.call('POST', '/v1/auth/sms/verify', {
    phone: '13900000001', challenge_id: sent.body.challenge_id,
    code: setup.messages[0].code
  });
  assert.equal(login.statusCode, 200);
  setup.advance(31 * 24 * 60 * 60 * 1000);
  await setup.call('GET', '/v1/auth/me');
  assert.equal(setup.db.prepare('SELECT COUNT(*) AS count FROM sms_challenges').get().count, 0);
  assert.equal(setup.db.prepare('SELECT COUNT(*) AS count FROM sessions').get().count, 1);
  setup.advance(30 * 24 * 60 * 60 * 1000);
  await setup.call('GET', '/v1/auth/me');
  assert.equal(setup.db.prepare('SELECT COUNT(*) AS count FROM sessions').get().count, 0);
  setup.db.close();
});

test('five device slots coexist and a new login replaces only its own slot', async () => {
  const setup = fixture();
  const ios = await signInWithSms(setup);
  setup.advance(61_000);
  const android = await signInWithSms(setup, '13900000001',
    { device_slot: 'android_phone' });
  setup.advance(61_000);
  const tablet = await signInWithSms(setup, '13900000001',
    { device_slot: 'tablet', device_platform: 'android' });
  assert.equal(tablet.body.device_platform, 'android');
  const macRequest = await requestDesktop(setup, 'mac');
  assert.equal((await setup.call('POST', '/v1/auth/desktop/approve',
    { request_id: macRequest.request.body.request_id }, ios.body.access_token)).statusCode,
  200);
  const mac = await setup.call('POST', '/v1/auth/desktop/claim', {
    request_id: macRequest.request.body.request_id, verifier: macRequest.proof.verifier
  });
  const windowsRequest = await requestDesktop(setup, 'windows');
  assert.equal((await setup.call('POST', '/v1/auth/desktop/approve',
    { request_id: windowsRequest.request.body.request_id }, ios.body.access_token)).statusCode,
  200);
  const windows = await setup.call('POST', '/v1/auth/desktop/claim', {
    request_id: windowsRequest.request.body.request_id, verifier: windowsRequest.proof.verifier
  });
  for (const session of [ios, android, tablet, mac, windows]) {
    assert.equal(session.statusCode, 200);
    assert.equal(session.body.user.id, ios.body.user.id);
    assert.equal(session.body.user.im_user_id, ios.body.user.im_user_id);
    assert.equal((await setup.call('GET', '/v1/auth/me', {},
      session.body.access_token)).statusCode, 200);
  }
  assert.equal(setup.db.prepare(`SELECT COUNT(*) AS count FROM sessions
    WHERE revoked_at IS NULL AND device_slot IS NOT NULL`).get().count, 5);

  setup.advance(61_000);
  const replacement = await signInWithSms(setup, '13900000001',
    { device_slot: 'tablet', device_platform: 'ios' });
  assert.equal(replacement.statusCode, 200);
  assert.equal(replacement.body.device_platform, 'ios');
  assert.equal((await setup.call('GET', '/v1/auth/me', {},
    tablet.body.access_token)).body.code, 'SESSION_REPLACED');
  assert.equal((await setup.call('POST', '/v1/auth/refresh', {
    refresh_token: tablet.body.refresh_token
  })).body.code, 'SESSION_REPLACED');
  for (const session of [ios, android, mac, windows, replacement])
    assert.equal((await setup.call('GET', '/v1/auth/me', {},
      session.body.access_token)).statusCode, 200);
  assert.equal(setup.db.prepare(`SELECT COUNT(*) AS count FROM sessions
    WHERE revoked_at IS NULL AND device_slot IS NOT NULL`).get().count, 5);
  setup.db.close();
});

test('mobile slot is validated before consuming a code and legacy sessions cannot approve QR', async () => {
  const setup = fixture();
  const sent = await setup.call('POST', '/v1/auth/sms/challenge',
    { phone: '13900000001' });
  const base = { phone: '13900000001', challenge_id: sent.body.challenge_id,
    code: setup.messages[0].code };
  for (const device of [{ device_slot: 'mac' }, { device_slot: 'tablet' },
    { device_slot: 'ios_phone', device_platform: 'android' }]) {
    const invalid = await setup.call('POST', '/v1/auth/sms/verify',
      { ...base, ...device });
    assert.equal(invalid.statusCode, 400);
  }
  const legacy = await setup.call('POST', '/v1/auth/sms/verify', base);
  assert.equal(legacy.statusCode, 200);
  assert.equal(legacy.body.device_slot, null);
  const { request } = await requestDesktop(setup, 'mac');
  const info = await setup.call('GET',
    `/v1/auth/desktop/request/${request.body.request_id}`, {},
    legacy.body.access_token);
  assert.equal(info.body.code, 'MOBILE_SESSION_REQUIRED');
  const approval = await setup.call('POST', '/v1/auth/desktop/approve',
    { request_id: request.body.request_id }, legacy.body.access_token);
  assert.equal(approval.body.code, 'MOBILE_SESSION_REQUIRED');
  setup.db.close();
});

test('device slot cutover requires mobile slots on every login path', async () => {
  const setup = fixture({ requireDeviceSlot: true });
  const sent = await setup.call('POST', '/v1/auth/sms/challenge',
    { phone: '13900000001' });
  const verify = { phone: '13900000001', challenge_id: sent.body.challenge_id,
    code: setup.messages[0].code };
  const missing = await setup.call('POST', '/v1/auth/sms/verify', verify);
  assert.equal(missing.statusCode, 400);
  assert.equal(missing.body.code, 'DEVICE_SLOT_REQUIRED');
  assert.equal(setup.db.prepare('SELECT COUNT(*) AS count FROM users').get().count, 0);
  assert.equal((await setup.call('POST', '/v1/auth/sms/verify', {
    ...verify, device_slot: 'windows'
  })).statusCode, 400);
  const ios = await setup.call('POST', '/v1/auth/sms/verify', {
    ...verify, device_slot: 'ios_phone'
  });
  assert.equal(ios.statusCode, 200);
  const password = 'another long account passphrase';
  assert.equal((await setup.call('POST', '/v1/auth/password/set', {
    new_password: password
  }, ios.body.access_token)).statusCode, 200);
  const passwordMissing = await setup.call('POST', '/v1/auth/password/login', {
    phone: '13900000001', password
  });
  assert.equal(passwordMissing.body.code, 'DEVICE_SLOT_REQUIRED');
  const android = await setup.call('POST', '/v1/auth/password/login', {
    phone: '13900000001', password, device_slot: 'android_phone'
  });
  assert.equal(android.statusCode, 200);
  assert.equal(android.body.user.id, ios.body.user.id);
  setup.advance(61_000);
  const reset = await setup.call('POST', '/v1/auth/sms/challenge', {
    phone: '13900000001', purpose: 'reset'
  });
  const resetBody = { phone: '13900000001', challenge_id: reset.body.challenge_id,
    code: setup.messages.at(-1).code,
    new_password: 'a third long account passphrase' };
  const resetMissing = await setup.call('POST', '/v1/auth/password/reset', resetBody);
  assert.equal(resetMissing.body.code, 'DEVICE_SLOT_REQUIRED');
  const tablet = await setup.call('POST', '/v1/auth/password/reset', {
    ...resetBody, device_slot: 'tablet', device_platform: 'ios'
  });
  assert.equal(tablet.statusCode, 200);
  assert.equal(tablet.body.device_slot, 'tablet');
  assert.equal(tablet.body.user.im_user_id, ios.body.user.im_user_id);
  setup.db.close();
});

test('cutover revokes existing unclassified sessions and rejects late legacy writes', async () => {
  const setup = fixture();
  const old = await signInWithSms(setup, '13900000001', {});
  assert.equal(old.body.device_slot, null);
  const strict = createAuthService({
    db: setup.db,
    sms: { async send(phone, code) { setup.messages.push({ phone, code }); } },
    secret: 'test-only-auth-hmac-key-keep-on-server-123456',
    imSecret: 'test-only-tencent-im-key-keep-on-server-123456',
    sdkAppId: 123456789,
    now: () => 1_780_000_000_000,
    requireDeviceSlot: true
  });
  const strictCall = (method, path, body = {}, token = '') => strict.handle({
    method, path, body, ip: '192.0.2.1',
    headers: token ? { authorization: `Bearer ${token}` } : {}
  });
  assert.equal(setup.db.prepare('SELECT revoked_reason FROM sessions WHERE access_hash=?')
    .get(createHash('sha256').update(old.body.access_token).digest('hex'))
    .revoked_reason, 'device_slot_required');
  const access = await strictCall('GET', '/v1/auth/me', {}, old.body.access_token);
  assert.equal(access.statusCode, 401);
  assert.equal(access.body.code, 'DEVICE_SLOT_REQUIRED');
  assert.equal((await strictCall('POST', '/v1/auth/refresh', {
    refresh_token: old.body.refresh_token
  })).body.code, 'DEVICE_SLOT_REQUIRED');

  setup.advance(61_000);
  const late = await signInWithSms(setup, '13900000001', {});
  assert.equal(late.statusCode, 200);
  assert.equal((await strictCall('GET', '/v1/auth/me', {},
    late.body.access_token)).body.code, 'DEVICE_SLOT_REQUIRED');
  assert.equal(setup.db.prepare('SELECT revoked_reason FROM sessions WHERE access_hash=?')
    .get(createHash('sha256').update(late.body.access_token).digest('hex'))
    .revoked_reason, 'device_slot_required');
  setup.db.close();
});

test('unconfigured or invalid social credentials cannot create accounts', async () => {
  const setup = fixture();
  const wechatPath = '/v1/auth/social/wechat/login';
  const qqPath = '/v1/auth/social/qq/login';
  assert.equal((await setup.rawCall('POST', wechatPath, {
    code: 'one-time-code', device_slot: 'ios_phone'
  })).statusCode, 403);
  assert.equal((await setup.call('POST', wechatPath, {
    code: 'one-time-code', device_slot: 'ios_phone', openid: 'forged'
  })).body.code, 'SOCIAL_PROVIDER_UNAVAILABLE');
  assert.equal((await setup.call('POST', qqPath, {
    access_token: 'sdk-token', device_slot: 'android_phone', openid: 'forged'
  })).body.code, 'SOCIAL_PROVIDER_UNAVAILABLE');
  assert.equal((await setup.call('POST', wechatPath, {
    code: 'one-time-code', device_slot: 'mac'
  })).statusCode, 400);
  assert.equal((await setup.call('POST', wechatPath, {
    code: 'one-time-code'
  })).statusCode, 400);
  assert.equal(setup.db.prepare('SELECT COUNT(*) AS count FROM users').get().count, 0);
  assert.equal(setup.db.prepare('SELECT COUNT(*) AS count FROM social_identities').get().count,
    0);
  setup.db.close();
});

test('WeChat and QQ bind through SMS to the same phone account and IM identity', async () => {
  const setup = fixture({ socialVerifiers: {
    wechat: async code => code.startsWith('wechat-ok') ?
      { appId: 'wx-app', subject: 'verified-wechat-openid' } : null,
    qq: async token => token.startsWith('qq-ok') ?
      { appId: 'qq-app', subject: 'verified-qq-openid' } : null
  } });
  const phone = '13900000001';
  const original = await signInWithSms(setup, phone, { device_slot: 'android_phone' });
  const wechatPath = '/v1/auth/social/wechat/login';
  const qqPath = '/v1/auth/social/qq/login';
  const wechat = await setup.call('POST', wechatPath, {
    code: 'wechat-ok-first', device_slot: 'ios_phone', openid: 'forged'
  });
  assert.equal(wechat.statusCode, 202);
  assert.equal(wechat.body.phone_binding_required, true);
  assert.equal(wechat.body.expires_in, 600);
  assert.equal(setup.db.prepare('SELECT COUNT(*) AS count FROM social_identities').get().count,
    0);
  assert.equal(setup.db.prepare('SELECT COUNT(*) AS count FROM sessions').get().count, 1);

  setup.advance(61_000);
  const wrongPurpose = await setup.call('POST', '/v1/auth/sms/challenge', { phone });
  assert.equal(wrongPurpose.statusCode, 200);
  assert.equal((await setup.call('POST', '/v1/auth/social/bind', {
    binding_token: wechat.body.binding_token, phone,
    challenge_id: wrongPurpose.body.challenge_id, code: setup.messages.at(-1).code
  })).statusCode, 400);
  setup.advance(61_000);
  const wechatSms = await setup.call('POST', '/v1/auth/sms/challenge', {
    phone, purpose: 'social_bind'
  });
  const wechatBindBody = { binding_token: wechat.body.binding_token, phone,
    challenge_id: wechatSms.body.challenge_id, code: setup.messages.at(-1).code };
  assert.equal((await setup.rawCall('POST', '/v1/auth/social/bind',
    wechatBindBody)).statusCode, 403);
  const boundWechat = await setup.call('POST', '/v1/auth/social/bind', wechatBindBody);
  assert.equal(boundWechat.statusCode, 200);
  assert.equal(boundWechat.body.device_slot, 'ios_phone');
  assert.equal(boundWechat.body.user.id, original.body.user.id);
  assert.equal(boundWechat.body.user.im_user_id, original.body.user.im_user_id);
  assert.equal((await setup.call('POST', '/v1/auth/social/bind',
    wechatBindBody)).body.code, 'SOCIAL_BINDING_EXPIRED');
  const stored = setup.db.prepare(`SELECT subject_hash FROM social_identities
    WHERE provider='wechat'`).get();
  assert.notEqual(stored.subject_hash, 'verified-wechat-openid');
  const again = await setup.call('POST', wechatPath, {
    code: 'wechat-ok-new-code', device_slot: 'ios_phone'
  });
  assert.equal(again.statusCode, 200);
  assert.equal(again.body.user.im_user_id, original.body.user.im_user_id);
  assert.equal((await setup.call('GET', '/v1/auth/me', {},
    boundWechat.body.access_token)).body.code, 'SESSION_REPLACED');

  const qq = await setup.call('POST', qqPath, {
    access_token: 'qq-ok-token', device_slot: 'tablet', device_platform: 'android'
  });
  assert.equal(qq.statusCode, 202);
  setup.advance(61_000);
  const qqSms = await setup.call('POST', '/v1/auth/sms/challenge', {
    phone, purpose: 'social_bind'
  });
  const boundQq = await setup.call('POST', '/v1/auth/social/bind', {
    binding_token: qq.body.binding_token, phone,
    challenge_id: qqSms.body.challenge_id, code: setup.messages.at(-1).code
  });
  assert.equal(boundQq.statusCode, 200);
  assert.equal(boundQq.body.device_slot, 'tablet');
  assert.equal(boundQq.body.device_platform, 'android');
  assert.equal(boundQq.body.user.id, original.body.user.id);
  assert.equal(boundQq.body.user.im_user_id, original.body.user.im_user_id);
  assert.equal(setup.db.prepare('SELECT COUNT(*) AS count FROM social_identities').get().count,
    2);
  setup.db.close();
});

test('social binding cannot move an identity to another phone or replace another provider ID', async () => {
  const setup = fixture({ socialVerifiers: {
    qq: async token => ({ appId: 'qq-app',
      subject: token === 'first-token' ? 'first-qq-id' : 'second-qq-id' })
  } });
  const login = token => setup.call('POST', '/v1/auth/social/qq/login', {
    access_token: token, device_slot: 'android_phone'
  });
  const firstIntent = await login('first-token');
  const competingIntent = await login('first-token');
  assert.equal(firstIntent.statusCode, 202);
  assert.equal(competingIntent.statusCode, 202);
  const firstPhone = '13900000001';
  const firstSms = await setup.call('POST', '/v1/auth/sms/challenge', {
    phone: firstPhone, purpose: 'social_bind'
  });
  const firstBind = await setup.call('POST', '/v1/auth/social/bind', {
    binding_token: firstIntent.body.binding_token, phone: firstPhone,
    challenge_id: firstSms.body.challenge_id, code: setup.messages.at(-1).code
  });
  assert.equal(firstBind.statusCode, 200);
  const secondPhone = '13900000002';
  const secondSms = await setup.call('POST', '/v1/auth/sms/challenge', {
    phone: secondPhone, purpose: 'social_bind'
  });
  const competing = await setup.call('POST', '/v1/auth/social/bind', {
    binding_token: competingIntent.body.binding_token, phone: secondPhone,
    challenge_id: secondSms.body.challenge_id, code: setup.messages.at(-1).code
  });
  assert.equal(competing.body.code, 'SOCIAL_IDENTITY_ALREADY_BOUND');
  assert.equal(setup.db.prepare('SELECT COUNT(*) AS count FROM users').get().count, 1);
  const otherIdentity = await login('second-token');
  assert.equal(otherIdentity.statusCode, 202);
  setup.advance(61_000);
  const anotherSms = await setup.call('POST', '/v1/auth/sms/challenge', {
    phone: firstPhone, purpose: 'social_bind'
  });
  const otherBind = await setup.call('POST', '/v1/auth/social/bind', {
    binding_token: otherIdentity.body.binding_token, phone: firstPhone,
    challenge_id: anotherSms.body.challenge_id, code: setup.messages.at(-1).code
  });
  assert.equal(otherBind.body.code, 'SOCIAL_PROVIDER_ALREADY_BOUND');
  assert.equal(setup.db.prepare('SELECT COUNT(*) AS count FROM social_identities').get().count,
    1);
  setup.db.close();
});

test('invalid provider proof, expired binding and repeated verification fail closed', async () => {
  let verifications = 0;
  const setup = fixture({ socialVerifiers: {
    wechat: async code => {
      ++verifications;
      if (code === 'invalid') return null;
      if (code === 'upstream-error') throw new Error('provider unavailable');
      return { appId: 'wx-app', subject: 'verified-openid' };
    }
  } });
  const path = '/v1/auth/social/wechat/login';
  const body = code => ({ code, device_slot: 'ios_phone' });
  assert.equal((await setup.call('POST', path,
    body('invalid'))).body.code, 'SOCIAL_CREDENTIAL_INVALID');
  assert.equal((await setup.call('POST', path,
    body('upstream-error'))).body.code, 'SOCIAL_PROVIDER_UNAVAILABLE');
  const unbound = await setup.call('POST', path, body('valid'));
  assert.equal(unbound.statusCode, 202);
  setup.advance(10 * 60_000);
  assert.equal((await setup.call('POST', '/v1/auth/social/bind', {
    binding_token: unbound.body.binding_token, phone: '13900000001',
    challenge_id: '00000000-0000-0000-0000-000000000000', code: '000000'
  })).body.code, 'SOCIAL_BINDING_EXPIRED');
  for (let attempt = 0; attempt < 30; ++attempt)
    assert.equal((await setup.call('POST', path,
      body('invalid'))).statusCode, 401);
  assert.equal((await setup.call('POST', path, body('invalid'))).statusCode, 429);
  assert.equal(verifications, 33);
  assert.equal(setup.db.prepare('SELECT COUNT(*) AS count FROM users').get().count, 0);
  setup.db.close();
});

test('desktop QR claim requires a mobile approval and original proof, then works once', async () => {
  const setup = fixture();
  const mobile = await signInWithSms(setup);
  const { proof, request } = await requestDesktop(setup, 'windows');
  assert.equal(request.body.expires_in, 120);
  assert.equal(new URL(request.body.qr_uri).searchParams.get('request_id'),
    request.body.request_id);
  const png = Buffer.from(request.body.qr_png_base64, 'base64');
  assert.equal(png.subarray(0, 8).toString('hex'), '89504e470d0a1a0a');
  assert.ok(png.length < 16 * 1024);
  assert.deepEqual(png, Buffer.from(await generatePngQrCode(
    request.body.qr_uri, { scale: 5, margin: 4 })));
  const infoPath = `/v1/auth/desktop/request/${request.body.request_id}`;
  assert.equal((await setup.call('GET', infoPath)).statusCode, 401);
  assert.deepEqual((await setup.call('POST', '/v1/auth/desktop/claim', {
    request_id: request.body.request_id, verifier: proof.verifier
  })).body, { status: 'pending', retry_after: 2 });
  const info = await setup.call('GET', infoPath, {}, mobile.body.access_token);
  assert.deepEqual({ platform: info.body.platform, display_code: info.body.display_code },
    { platform: 'windows', display_code: request.body.display_code });
  assert.equal(info.body.status, 'scanned');
  assert.deepEqual((await setup.call('POST', '/v1/auth/desktop/claim', {
    request_id: request.body.request_id, verifier: proof.verifier
  })).body, { status: 'scanned', retry_after: 2 });
  assert.equal((await setup.call('POST', '/v1/auth/desktop/claim', {
    request_id: request.body.request_id, verifier: desktopProof().verifier
  })).statusCode, 401);
  assert.equal((await setup.call('POST', '/v1/auth/desktop/approve', {
    request_id: request.body.request_id
  }, mobile.body.access_token)).statusCode, 200);
  const claims = await Promise.all([0, 1].map(() => setup.call('POST',
    '/v1/auth/desktop/claim', {
      request_id: request.body.request_id, verifier: proof.verifier
    })));
  assert.deepEqual(claims.map(claim => claim.statusCode).sort(), [200, 410]);
  const desktop = claims.find(claim => claim.statusCode === 200);
  assert.equal(desktop.body.device_slot, 'windows');
  assert.equal(desktop.body.user.id, mobile.body.user.id);
  assert.equal((await setup.call('GET', '/v1/auth/me', {},
    desktop.body.access_token)).statusCode, 200);
  assert.equal((await setup.call('POST', '/v1/auth/desktop/approve', {
    request_id: request.body.request_id
  }, desktop.body.access_token)).body.code, 'MOBILE_SESSION_REQUIRED');
  setup.db.close();
});

test('scanning does not reserve approval for the first mobile account', async () => {
  const setup = fixture();
  const scanner = await signInWithSms(setup, '13900000001');
  const approver = await signInWithSms(setup, '13900000002',
    { device_slot: 'android_phone' });
  const { proof, request } = await requestDesktop(setup, 'mac');
  const infoPath = `/v1/auth/desktop/request/${request.body.request_id}`;
  assert.equal((await setup.call('GET', infoPath, {},
    scanner.body.access_token)).body.status, 'scanned');
  assert.equal((await setup.call('GET', infoPath, {},
    approver.body.access_token)).body.status, 'scanned');
  const afterScan = setup.db.prepare(`SELECT status,approved_user_id
    FROM desktop_login_requests WHERE id=?`).get(request.body.request_id);
  assert.equal(afterScan.status, 'scanned');
  assert.equal(afterScan.approved_user_id, null);
  assert.equal((await setup.call('POST', '/v1/auth/desktop/approve', {
    request_id: request.body.request_id
  }, approver.body.access_token)).statusCode, 200);
  assert.equal((await setup.call('POST', '/v1/auth/desktop/approve', {
    request_id: request.body.request_id
  }, scanner.body.access_token)).statusCode, 409);
  const claimed = await setup.call('POST', '/v1/auth/desktop/claim', {
    request_id: request.body.request_id, verifier: proof.verifier
  });
  assert.equal(claimed.statusCode, 200);
  assert.equal(claimed.body.user.id, approver.body.user.id);

  const denied = await requestDesktop(setup, 'mac');
  assert.equal((await setup.call('GET',
    `/v1/auth/desktop/request/${denied.request.body.request_id}`, {},
    approver.body.access_token)).body.status, 'scanned');
  assert.equal((await setup.call('POST', '/v1/auth/desktop/deny', {
    request_id: denied.request.body.request_id
  }, scanner.body.access_token)).statusCode, 200);
  assert.equal((await setup.call('POST', '/v1/auth/desktop/claim', {
    request_id: denied.request.body.request_id, verifier: denied.proof.verifier
  })).statusCode, 403);
  assert.equal((await setup.call('POST', '/v1/auth/desktop/cancel', {
    request_id: denied.request.body.request_id, verifier: denied.proof.verifier
  })).statusCode, 409);
  setup.db.close();
});

test('desktop proof cancels pending, scanned or approved QR without issuing a session', async () => {
  const setup = fixture();
  const mobile = await signInWithSms(setup);
  let canceledBody;
  for (const stage of ['pending', 'scanned', 'approved']) {
    const { proof, request } = await requestDesktop(setup, 'mac');
    const requestId = request.body.request_id;
    const infoPath = `/v1/auth/desktop/request/${requestId}`;
    if (stage !== 'pending') {
      assert.equal((await setup.call('GET', infoPath, {},
        mobile.body.access_token)).body.status, 'scanned');
    }
    if (stage === 'approved') {
      assert.equal((await setup.call('POST', '/v1/auth/desktop/approve', {
        request_id: requestId
      }, mobile.body.access_token)).statusCode, 200);
    }
    assert.equal((await setup.call('POST', '/v1/auth/desktop/cancel', {
      request_id: requestId, verifier: desktopProof().verifier
    })).statusCode, 401);
    const cancelBody = { request_id: requestId, verifier: proof.verifier };
    canceledBody = cancelBody;
    assert.deepEqual((await setup.call('POST', '/v1/auth/desktop/cancel',
      cancelBody)).body, { status: 'canceled' });
    assert.deepEqual((await setup.call('POST', '/v1/auth/desktop/cancel',
      cancelBody)).body, { status: 'canceled' });
    assert.equal((await setup.call('GET', infoPath, {},
      mobile.body.access_token)).statusCode, 410);
    assert.equal((await setup.call('POST', '/v1/auth/desktop/approve', {
      request_id: requestId
    }, mobile.body.access_token)).statusCode, 410);
    assert.equal((await setup.call('POST', '/v1/auth/desktop/deny', {
      request_id: requestId
    }, mobile.body.access_token)).statusCode, 410);
    assert.equal((await setup.call('POST', '/v1/auth/desktop/claim',
      cancelBody)).statusCode, 410);
    assert.equal(setup.db.prepare('SELECT status FROM desktop_login_requests WHERE id=?')
      .get(requestId).status, 'canceled');
  }
  assert.equal(setup.db.prepare('SELECT COUNT(*) AS count FROM sessions').get().count, 1);
  setup.advance(120_000);
  assert.deepEqual((await setup.call('POST', '/v1/auth/desktop/cancel',
    canceledBody)).body, { status: 'canceled' });
  setup.db.close();
});

test('cancel after a lost claim response revokes only that exact desktop session', async () => {
  const setup = fixture();
  const mobile = await signInWithSms(setup);
  async function claimMac() {
    const pair = await requestDesktop(setup, 'mac');
    assert.equal((await setup.call('POST', '/v1/auth/desktop/approve', {
      request_id: pair.request.body.request_id
    }, mobile.body.access_token)).statusCode, 200);
    const claimed = await setup.call('POST', '/v1/auth/desktop/claim', {
      request_id: pair.request.body.request_id, verifier: pair.proof.verifier
    });
    assert.equal(claimed.statusCode, 200);
    return { ...pair, claimed };
  }
  const oldDesktop = await claimMac();
  const lostResponse = await claimMac();
  assert.equal((await setup.call('GET', '/v1/auth/me', {},
    oldDesktop.claimed.body.access_token)).body.code, 'SESSION_REPLACED');
  const lostRequestId = lostResponse.request.body.request_id;
  const claimedRow = setup.db.prepare(`SELECT status,claimed_session_id
    FROM desktop_login_requests WHERE id=?`).get(lostRequestId);
  assert.equal(claimedRow.status, 'consumed');
  assert.ok(claimedRow.claimed_session_id);
  const cancelBody = { request_id: lostRequestId,
    verifier: lostResponse.proof.verifier };
  assert.equal((await setup.call('POST', '/v1/auth/desktop/cancel', {
    ...cancelBody, verifier: desktopProof().verifier
  })).statusCode, 401);
  setup.advance(120_000);
  assert.deepEqual((await setup.call('POST', '/v1/auth/desktop/cancel',
    cancelBody)).body, { status: 'canceled' });
  assert.equal(setup.db.prepare('SELECT revoked_reason FROM sessions WHERE id=?')
    .get(claimedRow.claimed_session_id).revoked_reason, 'desktop_login_canceled');
  assert.equal((await setup.call('POST', '/v1/auth/desktop/claim',
    cancelBody)).statusCode, 410);
  const replacement = await claimMac();
  assert.deepEqual((await setup.call('POST', '/v1/auth/desktop/cancel',
    cancelBody)).body, { status: 'canceled' });
  assert.equal((await setup.call('GET', '/v1/auth/me', {},
    replacement.claimed.body.access_token)).statusCode, 200);
  assert.equal(setup.db.prepare('SELECT revoked_at FROM sessions WHERE id=?')
    .get(setup.db.prepare('SELECT claimed_session_id FROM desktop_login_requests WHERE id=?')
      .get(replacement.request.body.request_id).claimed_session_id).revoked_at, null);
  setup.db.close();
});

test('desktop denial, revoked approval, expiry and request rate limit block claims', async () => {
  const setup = fixture();
  const mobile = await signInWithSms(setup);
  const denied = await requestDesktop(setup, 'mac');
  assert.equal((await setup.call('POST', '/v1/auth/desktop/deny', {
    request_id: denied.request.body.request_id
  }, mobile.body.access_token)).statusCode, 200);
  assert.equal((await setup.call('POST', '/v1/auth/desktop/approve', {
    request_id: denied.request.body.request_id
  }, mobile.body.access_token)).statusCode, 409);
  assert.equal((await setup.call('POST', '/v1/auth/desktop/claim', {
    request_id: denied.request.body.request_id, verifier: denied.proof.verifier
  })).statusCode, 403);
  const revoked = await requestDesktop(setup, 'mac');
  assert.equal((await setup.call('POST', '/v1/auth/desktop/approve', {
    request_id: revoked.request.body.request_id
  }, mobile.body.access_token)).statusCode, 200);
  assert.equal((await setup.call('POST', '/v1/auth/logout', {},
    mobile.body.access_token)).statusCode, 200);
  assert.equal((await setup.call('POST', '/v1/auth/desktop/claim', {
    request_id: revoked.request.body.request_id, verifier: revoked.proof.verifier
  })).statusCode, 403);
  const expired = await requestDesktop(setup, 'mac');
  setup.advance(120_000);
  assert.equal((await setup.call('POST', '/v1/auth/desktop/claim', {
    request_id: expired.request.body.request_id, verifier: expired.proof.verifier
  })).statusCode, 410);
  assert.equal((await setup.call('POST', '/v1/auth/desktop/cancel', {
    request_id: expired.request.body.request_id, verifier: expired.proof.verifier
  })).statusCode, 410);
  for (let index = 0; index < 7; ++index) await requestDesktop(setup, 'mac');
  const limited = await setup.call('POST', '/v1/auth/desktop/request', {
    platform: 'mac', code_challenge: desktopProof().code_challenge
  });
  assert.equal(limited.statusCode, 429);
  setup.db.close();
});

test('late SMS completion cannot supersede a newer delivered code', async () => {
  const pending = [];
  const setup = fixture({ sms: { send(phone, code) {
    return new Promise(resolve => pending.push({ phone, code, resolve }));
  } } });
  const first = setup.call('POST', '/v1/auth/sms/challenge',
    { phone: '13900000001' });
  setup.advance(61_000);
  const second = setup.call('POST', '/v1/auth/sms/challenge',
    { phone: '13900000001' });
  assert.equal(pending.length, 2);
  pending[1].resolve();
  const latest = await second;
  pending[0].resolve();
  const old = await first;
  assert.equal((await setup.call('POST', '/v1/auth/sms/verify', {
    phone: '13900000001', challenge_id: old.body.challenge_id,
    code: pending[0].code, device_slot: 'ios_phone'
  })).statusCode, 400);
  assert.equal((await setup.call('POST', '/v1/auth/sms/verify', {
    phone: '13900000001', challenge_id: latest.body.challenge_id,
    code: pending[1].code, device_slot: 'ios_phone'
  })).statusCode, 200);
  setup.db.close();
});

test('existing SQLite session and QR tables migrate without losing rows', () => {
  const directory = mkdtempSync(join(tmpdir(), 'maichat-auth-test-'));
  const filename = join(directory, 'auth.sqlite');
  try {
    const old = new Database(filename);
    old.exec(`CREATE TABLE users (
      id TEXT PRIMARY KEY, phone_e164 TEXT NOT NULL UNIQUE,
      im_user_id TEXT NOT NULL UNIQUE, password_hash TEXT,
      created_at INTEGER NOT NULL, updated_at INTEGER NOT NULL, disabled_at INTEGER
    );
    INSERT INTO users VALUES ('user','+8613900000001','im-user',NULL,1,1,NULL);
    CREATE TABLE sessions (
      id TEXT PRIMARY KEY, user_id TEXT NOT NULL REFERENCES users(id),
      access_hash TEXT NOT NULL UNIQUE,
      access_expires_at INTEGER NOT NULL, refresh_hash TEXT NOT NULL UNIQUE,
      previous_refresh_hash TEXT, refresh_expires_at INTEGER NOT NULL,
      auth_time INTEGER NOT NULL, created_at INTEGER NOT NULL, revoked_at INTEGER
    );
    INSERT INTO sessions VALUES ('old','user','access',1,'refresh',NULL,2,3,4,NULL);
    CREATE TABLE desktop_login_requests (
      id TEXT PRIMARY KEY,
      platform TEXT NOT NULL CHECK(platform IN ('mac', 'windows')),
      code_challenge TEXT NOT NULL,
      display_code TEXT NOT NULL,
      ip_hash TEXT NOT NULL,
      status TEXT NOT NULL CHECK(status IN
        ('pending', 'scanned', 'approved', 'denied', 'consumed')),
      created_at INTEGER NOT NULL,
      expires_at INTEGER NOT NULL,
      approved_user_id TEXT REFERENCES users(id),
      approved_session_id TEXT REFERENCES sessions(id),
      approved_at INTEGER,
      consumed_at INTEGER
    );
    INSERT INTO desktop_login_requests VALUES
      ('old-request','mac','challenge','123456','ip','scanned',1,2,
        NULL,NULL,NULL,NULL);`);
    old.close();
    const migrated = openStore(filename);
    assert.equal(migrated.pragma('table_info(users)')
      .find(column => column.name === 'phone_e164').notnull, 0);
    const migratedUser = migrated.prepare('SELECT id,account_id FROM users WHERE id=?')
      .get('user');
    assert.equal(migratedUser.id, 'user');
    assert.match(migratedUser.account_id, /^m[0-9a-z]{20}$/);
    const row = migrated.prepare('SELECT * FROM sessions WHERE id=?').get('old');
    assert.equal(row.device_slot, null);
    assert.equal(row.revoked_reason, null);
    assert.equal(migrated.prepare('SELECT COUNT(*) AS count FROM sessions').get().count, 1);
    const request = migrated.prepare('SELECT * FROM desktop_login_requests WHERE id=?')
      .get('old-request');
    assert.equal(request.status, 'scanned');
    assert.equal(request.claimed_session_id, null);
    migrated.prepare("UPDATE desktop_login_requests SET status='canceled' WHERE id=?")
      .run('old-request');
    migrated.close();
    const reopened = openStore(filename);
    assert.equal(reopened.prepare('SELECT status FROM desktop_login_requests WHERE id=?')
      .get('old-request').status, 'canceled');
    reopened.close();
  } finally {
    rmSync(directory, { recursive: true, force: true });
  }
});

test('already canceled-capable QR tables gain the claimed session column', () => {
  const directory = mkdtempSync(join(tmpdir(), 'maichat-auth-qr-test-'));
  const filename = join(directory, 'auth.sqlite');
  try {
    const old = new Database(filename);
    old.exec(`CREATE TABLE desktop_login_requests (
      id TEXT PRIMARY KEY,
      platform TEXT NOT NULL CHECK(platform IN ('mac', 'windows')),
      code_challenge TEXT NOT NULL,
      display_code TEXT NOT NULL,
      ip_hash TEXT NOT NULL,
      status TEXT NOT NULL CHECK(status IN
        ('pending', 'scanned', 'approved', 'denied', 'consumed', 'canceled')),
      created_at INTEGER NOT NULL,
      expires_at INTEGER NOT NULL,
      approved_user_id TEXT,
      approved_session_id TEXT,
      approved_at INTEGER,
      consumed_at INTEGER
    );
    INSERT INTO desktop_login_requests VALUES
      ('old-request','mac','challenge','123456','ip','pending',1,2,
        NULL,NULL,NULL,NULL);`);
    old.close();
    const migrated = openStore(filename);
    const request = migrated.prepare('SELECT * FROM desktop_login_requests WHERE id=?')
      .get('old-request');
    assert.equal(request.status, 'pending');
    assert.equal(request.claimed_session_id, null);
    migrated.close();
  } finally {
    rmSync(directory, { recursive: true, force: true });
  }
});
