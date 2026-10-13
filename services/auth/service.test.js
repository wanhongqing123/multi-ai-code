'use strict';

const assert = require('node:assert/strict');
const { createHmac } = require('node:crypto');
const { inflateSync } = require('node:zlib');
const test = require('node:test');
const { openStore } = require('./store');
const { createAuthService } = require('./service');
const { bindLegacy } = require('./bind-legacy');
const { manageSmsAllowlist } = require('./manage-sms-allowlist');

function fixture(allowedPhones = ['+8613900000001'], extra = {}) {
  const db = openStore(':memory:');
  let current = 1_780_000_000_000;
  const messages = [];
  const service = createAuthService({
    db,
    sms: { async send(phone, code) { messages.push({ phone, code }); } },
    secret: 'test-only-auth-hmac-key-keep-on-server-123456',
    imSecret: 'test-only-tencent-im-key-keep-on-server-123456',
    sdkAppId: 123456789,
    allowedPhones,
    modelAllowedPhones: allowedPhones,
    now: () => current,
    ...extra
  });
  const rawCall = (method, path, body = {}, token = '') => service.handle({
    method, path, body, ip: '192.0.2.1',
    headers: token ? { authorization: `Bearer ${token}` } : {}
  });
  const ageCheckedPaths = new Set([
    '/v1/auth/sms/challenge', '/v1/auth/sms/verify',
    '/v1/auth/password/login', '/v1/auth/password/reset'
  ]);
  const call = (method, path, body = {}, token = '') => rawCall(
    method, path,
    method === 'POST' && ageCheckedPaths.has(path)
      ? { ...body, age_confirmed_14_plus: true } : body,
    token
  );
  return { db, messages, call, rawCall, advance(milliseconds) { current += milliseconds; } };
}

test('SMS allowlist changes take effect without recreating the auth service', async () => {
  const setup = fixture();
  const path = '/v1/auth/sms/challenge';
  const otherPhone = '13900000002';
  assert.equal((await setup.call('POST', path, { phone: otherPhone })).statusCode, 200);
  assert.equal(setup.messages.length, 0);
  assert.deepEqual(manageSmsAllowlist(setup.db, 'add', otherPhone),
    { allowed: true, changed: true });
  assert.equal((await setup.call('POST', path, { phone: otherPhone })).statusCode, 200);
  assert.equal(setup.messages.length, 1);
  setup.advance(61_000);
  assert.deepEqual(manageSmsAllowlist(setup.db, 'remove', otherPhone),
    { allowed: false, changed: true });
  assert.equal((await setup.call('POST', path, { phone: otherPhone })).statusCode, 200);
  assert.equal(setup.messages.length, 1);
  setup.db.close();
});

test('the old environment allowlist is imported only once', () => {
  const setup = fixture();
  assert.equal(manageSmsAllowlist(setup.db, 'check', '13900000001').allowed, true);
  manageSmsAllowlist(setup.db, 'remove', '13900000001');
  // 同一个数据库再次初始化时，已删除的号码不能被旧环境变量复活。
  createAuthService({
    db: setup.db,
    sms: { async send() {} },
    secret: 'test-only-auth-hmac-key-keep-on-server-123456',
    imSecret: 'test-only-tencent-im-key-keep-on-server-123456',
    sdkAppId: 123456789,
    allowedPhones: ['+8613900000001']
  });
  assert.equal(manageSmsAllowlist(setup.db, 'check', '13900000001').allowed, false);
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

test('only an allowed signed-in account receives short-lived Seedance credentials', async () => {
  let issued = 0;
  const setup = fixture(undefined, { issueArkCredential: async () => ({
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

  const denied = fixture(undefined, { modelAllowedPhones: ['+8613900000002'],
    issueArkCredential: async () => { throw new Error('must not issue'); } });
  const request = await denied.call('POST', '/v1/auth/sms/challenge',
    { phone: '13900000001' });
  const account = await denied.call('POST', '/v1/auth/sms/verify', {
    phone: '13900000001', challenge_id: request.body.challenge_id,
    code: denied.messages[0].code
  });
  assert.equal((await denied.call('GET', path, {}, account.body.access_token)).statusCode, 403);
  denied.db.close();
});

test('only a signed-in model account receives cached short-lived Bailian credentials', async () => {
  let issued = 0;
  const setup = fixture(undefined, { issueBailianCredential: async () => ({
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
  const denied = fixture(undefined, { modelAllowedPhones: [],
    issueBailianCredential: async () => { throw new Error('must not issue'); } });
  const deniedChallenge = await denied.call('POST', '/v1/auth/sms/challenge',
    { phone: '13900000001' });
  const account = await denied.call('POST', '/v1/auth/sms/verify', {
    phone: '13900000001', challenge_id: deniedChallenge.body.challenge_id,
    code: denied.messages[0].code
  });
  assert.equal((await denied.call('GET', path, {}, account.body.access_token)).statusCode, 403);
  denied.db.close();
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
    { phone: '13900000001', password });
  assert.equal(logged.statusCode, 200);
  assert.equal(logged.body.user.id, initial.body.user.id);
  const rotated = await setup.call('POST', '/v1/auth/refresh',
    { refresh_token: logged.body.refresh_token });
  assert.equal(rotated.statusCode, 200);
  assert.notEqual(rotated.body.refresh_token, logged.body.refresh_token);
  assert.equal((await setup.call('GET', '/v1/auth/me', {},
    logged.body.access_token)).statusCode, 401);
  assert.equal((await setup.call('POST', '/v1/auth/logout', {},
    rotated.body.access_token)).statusCode, 200);
  assert.equal((await setup.call('GET', '/v1/auth/me', {},
    rotated.body.access_token)).statusCode, 401);
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

test('allowlist prevents SMS spending and password reset revokes old sessions', async () => {
  const setup = fixture();
  const ignored = await setup.call('POST', '/v1/auth/sms/challenge',
    { phone: '13900000002' });
  assert.equal(ignored.statusCode, 200);
  assert.equal(setup.messages.length, 0);
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
    code: setup.messages[1].code, new_password: 'another long passphrase for account'
  });
  assert.equal(changed.statusCode, 200);
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
