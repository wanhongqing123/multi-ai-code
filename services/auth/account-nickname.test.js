'use strict';

const assert = require('node:assert/strict');
const { randomBytes } = require('node:crypto');
const { mkdtempSync, rmSync } = require('node:fs');
const { tmpdir } = require('node:os');
const path = require('node:path');
const test = require('node:test');
const Database = require('better-sqlite3');
const { openStore } = require('./store');
const { createAuthService } = require('./service');

function fixture() {
  const db = openStore(':memory:');
  const service = createAuthService({ db,
    sms: { async send() {} }, secret: randomBytes(32).toString('hex'),
    imSecret: randomBytes(32).toString('hex'), sdkAppId: 123456789 });
  const call = (method, path, body = {}, token = '') => service.handle({ method, path,
    body, ip: '127.0.0.1', headers: token ? { authorization: `Bearer ${token}` } : {} });
  return { db, service, call };
}

async function login(setup, id, slot) {
  return (await setup.call('POST', '/v1/auth/password/login', {
    account_id: id, password: 'nickname-test-password-123456',
    age_confirmed_14_plus: true, device_slot: slot
  })).body;
}

test('MaiChat nickname is editable account data exposed to both sides of friendship', async () => {
  const setup = fixture();
  for (const id of ['test_alice', 'test_bob'])
    assert.equal((await setup.service.createTestAccount(id,
      'nickname-test-password-123456')).statusCode, 201);
  const alice = await login(setup, 'test_alice', 'ios_phone');
  const bob = await login(setup, 'test_bob', 'android_phone');
  assert.equal(alice.user.nickname, null);
  assert.equal((await setup.call('GET', '/v1/auth/me', {}, alice.access_token))
    .body.user.nickname, null);
  assert.equal((await setup.call('POST', '/v1/auth/profile',
    { nickname: '  小王  ' })).statusCode, 401);
  const updated = await setup.call('POST', '/v1/auth/profile',
    { nickname: '  小王  ' }, alice.access_token);
  assert.equal(updated.statusCode, 200);
  assert.equal(updated.body.user.nickname, '小王');
  assert.equal(updated.body.user.id, 'test_alice');
  assert.equal(updated.body.user.im_user_id, 'test_alice');
  assert.equal(setup.db.prepare('SELECT nickname FROM users WHERE id=?')
    .get('test_alice').nickname, '小王');
  assert.equal(setup.db.prepare('SELECT desired_nickname FROM profile_im_sync')
    .get().desired_nickname, '小王');
  const refreshed = await setup.call('POST', '/v1/auth/refresh',
    { refresh_token: alice.refresh_token });
  assert.equal(refreshed.body.user.nickname, '小王');
  assert.equal((await setup.call('GET', '/v1/auth/me', {}, refreshed.body.access_token))
    .body.user.nickname, '小王');
  const resolved = await setup.call('POST', '/v1/friends/resolve',
    { account: 'test_alice' }, bob.access_token);
  assert.equal(resolved.body.nickname, '小王');
  const request = await setup.call('POST', '/v1/friends/requests',
    { account: 'test_bob' }, refreshed.body.access_token);
  const incoming = await setup.call('GET', '/v1/friends/requests', {}, bob.access_token);
  assert.equal(incoming.body.incoming[0].nickname, '小王');
  await setup.call('POST', `/v1/friends/requests/${request.body.request_id}/accept`,
    {}, bob.access_token);
  const friends = await setup.call('GET', '/v1/friends', {}, bob.access_token);
  assert.equal(friends.body.friends[0].nickname, '小王');
  setup.db.close();
});

test('nickname rejects invalid values and clearing it keeps the stable account ID', async () => {
  const setup = fixture();
  await setup.service.createTestAccount('test_alice', 'nickname-test-password-123456');
  const alice = await login(setup, 'test_alice', 'ios_phone');
  for (const nickname of [null, 123, 'a'.repeat(41), 'a\nb', 'a\u0000b']) {
    assert.equal((await setup.call('POST', '/v1/auth/profile',
      { nickname }, alice.access_token)).statusCode, 400);
  }
  assert.equal((await setup.call('POST', '/v1/auth/profile',
    {}, alice.access_token)).statusCode, 400);
  assert.equal((await setup.call('POST', '/v1/auth/profile',
    { nickname: 'Alice' }, alice.access_token)).body.user.nickname, 'Alice');
  const cleared = await setup.call('POST', '/v1/auth/profile',
    { nickname: '   ' }, alice.access_token);
  assert.equal(cleared.statusCode, 200);
  assert.equal(cleared.body.user.id, 'test_alice');
  assert.equal(cleared.body.user.nickname, null);
  assert.equal(setup.db.prepare('SELECT desired_nickname FROM profile_im_sync')
    .get().desired_nickname, '');
  setup.db.close();
});

test('existing phone accounts migrate without losing the IM identity', () => {
  const directory = mkdtempSync(path.join(tmpdir(), 'maichat-nickname-'));
  const filename = path.join(directory, 'auth.sqlite');
  try {
    const legacy = new Database(filename);
    legacy.exec(`CREATE TABLE users (
      id TEXT PRIMARY KEY, phone_e164 TEXT NOT NULL UNIQUE,
      im_user_id TEXT NOT NULL UNIQUE, password_hash TEXT,
      created_at INTEGER NOT NULL, updated_at INTEGER NOT NULL, disabled_at INTEGER
    );
    INSERT INTO users(id,phone_e164,im_user_id,created_at,updated_at)
      VALUES('legacy-account','+8613900000001','legacy-im-id',1,1);`);
    legacy.close();
    const migrated = openStore(filename);
    const user = migrated.prepare(`SELECT id,account_id,nickname,phone_e164,im_user_id
      FROM users`).get();
    assert.equal(user.id, 'legacy-account');
    assert.equal(user.nickname, null);
    assert.equal(user.phone_e164, '+8613900000001');
    assert.equal(user.im_user_id, 'legacy-im-id');
    assert.match(user.account_id, /^m[0-9a-z]{20}$/);
    assert.equal(migrated.pragma('foreign_key_check').length, 0);
    migrated.close();
  } finally {
    rmSync(directory, { recursive: true, force: true });
  }
});
