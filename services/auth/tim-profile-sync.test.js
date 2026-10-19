'use strict';

const assert = require('node:assert/strict');
const { randomBytes } = require('node:crypto');
const test = require('node:test');
const { openStore } = require('./store');
const { createAuthService } = require('./service');
const { createTencentProfileWriter, syncPendingProfileActions } =
  require('./tim-profile-sync');

test('Tencent profile writer projects only the MaiChat nickname', async () => {
  const requests = [];
  const writer = createTencentProfileWriter({ sdkAppId: 123456789,
    imSecret: randomBytes(32).toString('hex'), fetchImpl: async (url, options) => {
      requests.push({ url, options });
      return { ok: true, text: async () => JSON.stringify({ ActionStatus: 'OK',
        ErrorCode: 0 }) };
    } });
  await writer({ imUserId: 'test_alice', nickname: '小王' });
  assert.equal(requests[0].url.pathname, '/v4/profile/portrait_set');
  assert.deepEqual(JSON.parse(requests[0].options.body), {
    From_Account: 'test_alice',
    ProfileItem: [{ Tag: 'Tag_Profile_IM_Nick', Value: '小王' }]
  });
  await writer({ imUserId: 'test_alice', nickname: '' });
  assert.equal(JSON.parse(requests[1].options.body).ProfileItem[0].Value, '');
});

test('profile sync retries failure and never deletes a newer nickname revision', async () => {
  const db = openStore(':memory:');
  const service = createAuthService({ db, sms: { async send() {} },
    secret: randomBytes(32).toString('hex'),
    imSecret: randomBytes(32).toString('hex'), sdkAppId: 123456789 });
  await service.createTestAccount('test_alice', 'nickname-test-password-123456');
  const call = (method, path, body, token = '') => service.handle({ method, path,
    body, ip: '127.0.0.1', headers: token ? { authorization: `Bearer ${token}` } : {} });
  const login = await call('POST', '/v1/auth/password/login', {
    account_id: 'test_alice', password: 'nickname-test-password-123456',
    age_confirmed_14_plus: true, device_slot: 'ios_phone'
  });
  const token = login.body.access_token;
  await call('POST', '/v1/auth/profile', { nickname: '小王' }, token);
  assert.deepEqual(await syncPendingProfileActions(db, async () => {
    throw new Error('network unavailable');
  }), { pending: 1, completed: 0 });
  assert.equal(db.prepare('SELECT attempts,last_error FROM profile_im_sync')
    .get().attempts, 1);
  assert.equal(db.prepare('SELECT attempts,last_error FROM profile_im_sync')
    .get().last_error, 'IM sync unavailable');
  const observed = [];
  assert.deepEqual(await syncPendingProfileActions(db, async operation => {
    observed.push(operation.nickname);
    await call('POST', '/v1/auth/profile', { nickname: '新昵称' }, token);
  }), { pending: 1, completed: 0 });
  assert.equal(db.prepare('SELECT desired_nickname FROM profile_im_sync')
    .get().desired_nickname, '新昵称');
  assert.deepEqual(await syncPendingProfileActions(db, async operation => {
    observed.push(operation.nickname);
  }), { pending: 0, completed: 1 });
  assert.deepEqual(observed, ['小王', '新昵称']);
  assert.equal(db.prepare('SELECT COUNT(*) AS count FROM profile_im_sync').get().count, 0);
  db.close();
});
