'use strict';

const assert = require('node:assert/strict');
const { randomBytes } = require('node:crypto');
const test = require('node:test');
const { openStore } = require('./store');
const { createAuthService } = require('./service');
const { createTencentFriendWriter, syncPendingFriendActions } = require('./tim-friend-sync');
const { importFriendMirror } = require('./import-friend-mirror');
const { createTencentAccountProvisioner } = require('./tim-account-provision');

function fixture() {
  const db = openStore(':memory:');
  const codes = [];
  const service = createAuthService({ db,
    sms: { async send(phone, code) { codes.push({ phone, code }); } },
    secret: randomBytes(32).toString('hex'), imSecret: randomBytes(32).toString('hex'),
    sdkAppId: 123456789 });
  const call = (method, path, body = {}, token = '') => service.handle({ method, path,
    body, ip: '127.0.0.1', headers: token ? { authorization: `Bearer ${token}` } : {} });
  return { db, codes, service, call };
}

async function phoneAccount(setup, phone) {
  const challenge = await setup.call('POST', '/v1/auth/sms/challenge',
    { phone, age_confirmed_14_plus: true });
  assert.equal(challenge.statusCode, 200);
  const signedIn = await setup.call('POST', '/v1/auth/sms/verify', {
    phone, challenge_id: challenge.body.challenge_id, code: setup.codes.at(-1).code,
    age_confirmed_14_plus: true, device_slot: 'ios_phone'
  });
  assert.equal(signedIn.statusCode, 200);
  return signedIn.body;
}

test('admin-created account has no phone, uses its MaiChat ID for IM, and logs in by ID', async () => {
  const setup = fixture();
  const password = 'test-account-secret-123456';
  assert.equal((await setup.service.createTestAccount('test_alice', password)).statusCode, 201);
  const row = setup.db.prepare('SELECT * FROM users WHERE id=?').get('test_alice');
  assert.equal(row.phone_e164, null);
  assert.equal(row.im_user_id, 'test_alice');
  assert.notEqual(row.password_hash, password);
  assert.equal((await setup.call('POST', '/v1/auth/password/login', {
    account_id: 'test_alice', password: 'wrong', age_confirmed_14_plus: true,
    device_slot: 'android_phone'
  })).statusCode, 401);
  const login = await setup.call('POST', '/v1/auth/password/login', {
    account_id: 'test_alice', password, age_confirmed_14_plus: true,
    device_slot: 'android_phone'
  });
  assert.equal(login.statusCode, 200);
  assert.equal(login.body.user.id, 'test_alice');
  assert.equal(login.body.user.im_user_id, 'test_alice');
  const me = await setup.call('GET', '/v1/auth/me', {}, login.body.access_token);
  assert.equal(me.body.user.phone_masked, null);
  const desktop = await setup.call('POST', '/v1/auth/password/login', {
    account_id: 'test_alice', password, age_confirmed_14_plus: true,
    device_slot: 'windows', device_platform: 'windows'
  });
  assert.equal(desktop.statusCode, 200);
  assert.equal((await setup.service.createTestAccount('test_alice', password)).statusCode, 409);
  assert.equal(setup.service.disableTestAccount('test_alice').statusCode, 200);
  assert.equal((await setup.call('GET', '/v1/auth/me', {}, login.body.access_token)).statusCode, 401);
  assert.equal((await setup.call('POST', '/v1/auth/password/login', {
    account_id: 'test_alice', password, age_confirmed_14_plus: true,
    device_slot: 'ios_phone'
  })).statusCode, 401);
  setup.db.close();
});

test('MaiChat owns request, approval, list and deletion; IM changes follow the queue', async () => {
  const setup = fixture();
  const alice = await phoneAccount(setup, '13900000001');
  await setup.service.createTestAccount('test_bob', 'test-bob-secret-123456');
  const bob = (await setup.call('POST', '/v1/auth/password/login', {
    account_id: 'test_bob', password: 'test-bob-secret-123456',
    age_confirmed_14_plus: true, device_slot: 'android_phone'
  })).body;
  const aliceToken = alice.access_token;
  const bobToken = bob.access_token;
  assert.equal((await setup.call('GET', '/v1/friends')).statusCode, 401);
  assert.equal((await setup.call('POST', '/v1/friends/resolve',
    { account: 'test_bob' }, aliceToken)).body.account_id, 'test_bob');
  assert.equal((await setup.call('POST', '/v1/friends/resolve',
    { account: '13900000001' }, aliceToken)).statusCode, 400);
  const request = await setup.call('POST', '/v1/friends/requests',
    { account: 'test_bob' }, aliceToken);
  assert.equal(request.statusCode, 201);
  assert.equal((await setup.call('GET', '/v1/friends', {}, aliceToken)).body.friends.length, 0);
  assert.equal((await setup.call('GET', '/v1/friends/requests', {}, bobToken))
    .body.incoming[0].account_id, alice.user.id);
  assert.equal((await setup.call('POST', `/v1/friends/requests/${request.body.request_id}/accept`,
    {}, aliceToken)).statusCode, 404);
  assert.equal((await setup.call('POST', `/v1/friends/requests/${request.body.request_id}/accept`,
    {}, bobToken)).statusCode, 200);
  const list = await setup.call('GET', '/v1/friends', {}, aliceToken);
  assert.deepEqual(list.body.friends.map(friend => friend.account_id), ['test_bob']);
  assert.equal(list.body.friends[0].pending_im_action, 'add');
  const calls = [];
  assert.deepEqual(await syncPendingFriendActions(setup.db, async operation => {
    calls.push(operation);
  }), { pending: 0, completed: 1 });
  assert.equal(calls[0].action, 'add');
  assert.equal((await setup.call('GET', '/v1/friends', {}, bobToken))
    .body.friends[0].account_id, alice.user.id);
  assert.equal((await setup.call('POST', '/v1/friends/remove',
    { account: alice.user.id }, bobToken)).statusCode, 200);
  assert.equal((await setup.call('GET', '/v1/friends', {}, bobToken)).body.friends.length, 0);
  await syncPendingFriendActions(setup.db, async operation => { calls.push(operation); });
  assert.equal(calls[1].action, 'delete');
  setup.db.close();
});

test('IM writer failure keeps the MaiChat relation and retries without losing the desired state', async () => {
  const setup = fixture();
  await setup.service.createTestAccount('test_one', 'test-one-secret-123456');
  await setup.service.createTestAccount('test_two', 'test-two-secret-123456');
  const login = async account => (await setup.call('POST', '/v1/auth/password/login', {
    account_id: account, password: `test-${account.slice(5)}-secret-123456`,
    age_confirmed_14_plus: true, device_slot: 'ios_phone'
  })).body.access_token;
  const first = await login('test_one');
  const second = await login('test_two');
  const request = await setup.call('POST', '/v1/friends/requests',
    { account: 'test_two' }, first);
  await setup.call('POST', `/v1/friends/requests/${request.body.request_id}/accept`, {}, second);
  await syncPendingFriendActions(setup.db, async () => { throw new Error('network failure'); });
  assert.equal((await setup.call('GET', '/v1/friends', {}, first)).body.friends.length, 1);
  assert.equal(setup.db.prepare('SELECT attempts,last_error FROM friend_im_sync').get().attempts, 1);
  await syncPendingFriendActions(setup.db, async () => {});
  assert.equal(setup.db.prepare('SELECT COUNT(*) AS count FROM friend_im_sync').get().count, 0);
  setup.db.close();
});

test('Tencent writer applies a confirmed relation with admin REST payload only', async () => {
  const requests = [];
  const writer = createTencentFriendWriter({ sdkAppId: 123456789,
    imSecret: randomBytes(32).toString('hex'), fetchImpl: async (url, options) => {
      requests.push({ url, options });
      return { ok: true, text: async () => JSON.stringify({ ActionStatus: 'OK',
        ErrorCode: 0, ResultItem: [{ To_Account: 'test_two', ResultCode: 0 }] }) };
    } });
  await writer({ action: 'add', from: 'test_one', to: 'test_two' });
  await writer({ action: 'delete', from: 'test_one', to: 'test_two' });
  assert.equal(requests[0].url.pathname, '/v4/sns/friend_add');
  assert.equal(JSON.parse(requests[0].options.body).ForceAddFlags, 1);
  assert.equal(JSON.parse(requests[0].options.body).AddType, 'Add_Type_Both');
  assert.equal(requests[1].url.pathname, '/v4/sns/friend_delete');
  assert.equal(JSON.parse(requests[1].options.body).DeleteType, 'Delete_Type_Both');
});

test('Tencent writer confirms both-way friendship before treating duplicate add as synced', async () => {
  const paths = [];
  const writer = createTencentFriendWriter({ sdkAppId: 123456789,
    imSecret: randomBytes(32).toString('hex'), fetchImpl: async url => {
      paths.push(url.pathname);
      const result = url.pathname.endsWith('friend_add') ?
        { ResultItem: [{ To_Account: 'test_two', ResultCode: 30015 }] } :
        { InfoItem: [{ To_Account: 'test_two', ResultCode: 0,
          Relation: 'CheckResult_Type_BothWay' }] };
      return { ok: true, text: async () => JSON.stringify({
        ActionStatus: 'OK', ErrorCode: 0, ...result }) };
    } });
  await writer({ action: 'add', from: 'test_one', to: 'test_two' });
  assert.deepEqual(paths, ['/v4/sns/friend_add', '/v4/sns/friend_check']);
});

test('legacy reciprocal IM friends import once without reviving later deletions', async () => {
  const setup = fixture();
  for (const id of ['test_first', 'test_second', 'test_third'])
    await setup.service.createTestAccount(id, 'test-password-123456789');
  const addMirror = setup.db.prepare(`INSERT INTO friend_mirror
    (owner_user_id,friend_user_id,source,observed_at)
    VALUES(?,?,'tencent_im',1)`);
  addMirror.run('test_first', 'test_second');
  addMirror.run('test_second', 'test_first');
  addMirror.run('test_first', 'test_third');
  assert.equal(importFriendMirror(setup.db, { dryRun: true }).mutual_pairs, 1);
  assert.equal(importFriendMirror(setup.db, { now: () => 20 }).imported, 1);
  setup.db.prepare(`DELETE FROM friendships WHERE left_user_id='test_first'
    AND right_user_id='test_second'`).run();
  assert.equal(importFriendMirror(setup.db).imported, 0);
  assert.equal(setup.db.prepare('SELECT COUNT(*) AS count FROM friendships').get().count, 0);
  setup.db.close();
});

test('disabling a test account revokes sessions and queues removal of its friendships', async () => {
  const setup = fixture();
  await setup.service.createTestAccount('test_active', 'test-active-secret-123456');
  await setup.service.createTestAccount('test_disabled', 'test-disabled-secret-123456');
  setup.db.prepare(`INSERT INTO friendships(left_user_id,right_user_id,created_at)
    VALUES('test_active','test_disabled',1)`).run();
  const login = await setup.call('POST', '/v1/auth/password/login', {
    account_id: 'test_disabled', password: 'test-disabled-secret-123456',
    age_confirmed_14_plus: true, device_slot: 'android_phone'
  });
  assert.equal(login.statusCode, 200);
  assert.equal(setup.service.disableTestAccount('test_disabled').statusCode, 200);
  assert.equal((await setup.call('GET', '/v1/auth/me', {},
    login.body.access_token)).statusCode, 401);
  assert.equal(setup.db.prepare('SELECT COUNT(*) AS count FROM friendships').get().count, 0);
  assert.equal(setup.db.prepare('SELECT desired FROM friend_im_sync').get().desired, 'delete');
  setup.db.close();
});

test('manual test ID is not bound if an unrelated Tencent IM account already exists', async () => {
  const paths = [];
  const provision = createTencentAccountProvisioner({ sdkAppId: 123456789,
    imSecret: randomBytes(32).toString('hex'), fetchImpl: async url => {
      paths.push(url.pathname);
      return { ok: true, text: async () => JSON.stringify({ ActionStatus: 'OK', ErrorCode: 0,
        ResultItem: [{ UserID: 'test_taken', ResultCode: 0, AccountStatus: 'Imported' }] }) };
    } });
  assert.equal(await provision.checkStatus('test_taken'), 'Imported');
  assert.equal(await provision('test_taken'), false);
  assert.deepEqual(paths, [
    '/v4/im_open_login_svc/account_check', '/v4/im_open_login_svc/account_check']);
  const setup = fixture();
  const service = createAuthService({ db: setup.db,
    sms: { async send() {} }, secret: randomBytes(32).toString('hex'),
    imSecret: randomBytes(32).toString('hex'), sdkAppId: 123456789,
    provisionTestImAccount: provision });
  assert.equal((await service.createTestAccount('test_taken',
    'test-password-123456789')).statusCode, 409);
  assert.equal(setup.db.prepare('SELECT COUNT(*) AS count FROM users').get().count, 0);
  setup.db.close();
});
