'use strict';

const assert = require('node:assert/strict');
const { randomBytes } = require('node:crypto');
const test = require('node:test');
const { openStore } = require('./store');
const { createAuthService } = require('./service');
const { importCachedTestContacts } = require('./import-cached-test-contacts');

function fixture() {
  const db = openStore(':memory:');
  db.prepare(`INSERT INTO users
    (id,account_id,phone_e164,im_user_id,created_at,updated_at)
    VALUES('owner-internal','mowner','+8613900000001','owner-im',1,1)`).run();
  const manifest = { owner_account_id: 'mowner', owner_im_user_id: 'owner-im',
    contacts: [
      { im_user_id: 'test_alpha', expected_status: 'Imported' },
      { im_user_id: 'test_beta', expected_status: 'NotImported' }
    ] };
  const statuses = { test_alpha: 'Imported', test_beta: 'NotImported' };
  const imported = [];
  const options = { password: 'short1', now: () => 123,
    imStatus: async id => statuses[id],
    importUnused: async id => { imported.push(id); statuses[id] = 'Imported'; return true; } };
  return { db, manifest, options, imported, statuses };
}

test('dry run checks IM status without creating accounts or friendships', async () => {
  const setup = fixture();
  const result = await importCachedTestContacts(setup.db, setup.manifest,
    { ...setup.options, dryRun: true });
  assert.deepEqual(result, { mode: 'dry-run', contacts: 2, existing_im: 1,
    new_im: 1, friendships: 2 });
  assert.equal(setup.db.prepare('SELECT COUNT(*) AS n FROM users').get().n, 1);
  assert.equal(setup.db.prepare('SELECT COUNT(*) AS n FROM friendships').get().n, 0);
  assert.deepEqual(setup.imported, []);
  setup.db.close();
});

test('migration keeps existing IM IDs and makes cached contacts visible as MaiChat friends', async () => {
  const setup = fixture();
  const result = await importCachedTestContacts(setup.db, setup.manifest, setup.options);
  assert.deepEqual(result, { mode: 'run', contacts: 2, existing_im: 1,
    new_im: 1, friendships: 2 });
  assert.deepEqual(setup.imported, ['test_beta']);
  const rows = setup.db.prepare(`SELECT id,account_id,im_user_id,nickname,phone_e164,password_hash
    FROM users WHERE id LIKE 'test_%' ORDER BY id`).all();
  assert.deepEqual(rows.map(row => [row.id, row.account_id, row.im_user_id, row.nickname]), [
    ['test_alpha', 'test_alpha', 'test_alpha', '测试-test_alpha'],
    ['test_beta', 'test_beta', 'test_beta', '测试-test_beta']
  ]);
  assert(rows.every(row => row.phone_e164 === null && row.password_hash !== 'short1'));
  assert.equal(setup.db.prepare('SELECT COUNT(*) AS n FROM friendships').get().n, 2);
  assert.equal(setup.db.prepare('SELECT COUNT(*) AS n FROM friend_im_sync').get().n, 2);
  assert.equal(setup.db.prepare('SELECT COUNT(*) AS n FROM profile_im_sync').get().n, 2);
  const service = createAuthService({ db: setup.db, sms: { async send() {} },
    secret: randomBytes(32).toString('hex'), imSecret: randomBytes(32).toString('hex'),
    sdkAppId: 123456789 });
  const login = await service.handle({ method: 'POST', path: '/v1/auth/password/login',
    ip: '127.0.0.1', headers: {}, body: { account_id: 'test_alpha', password: 'short1',
      age_confirmed_14_plus: true, device_slot: 'ios_phone' } });
  assert.equal(login.statusCode, 200);
  const friends = await service.handle({ method: 'GET', path: '/v1/friends',
    ip: '127.0.0.1', headers: { authorization: `Bearer ${login.body.access_token}` }, body: {} });
  assert.equal(friends.statusCode, 200);
  assert.equal(friends.body.friends[0].account_id, 'mowner');
  assert.equal((await service.createTestAccount('test_new', 'short1')).statusCode, 400);
  await assert.rejects(importCachedTestContacts(setup.db, setup.manifest, setup.options),
    /already imported/);
  setup.db.close();
});

test('changed IM status or existing account aborts before writing', async () => {
  const setup = fixture();
  setup.statuses.test_alpha = 'NotImported';
  await assert.rejects(importCachedTestContacts(setup.db, setup.manifest, setup.options),
    /status changed/);
  assert.equal(setup.db.prepare('SELECT COUNT(*) AS n FROM users').get().n, 1);
  setup.statuses.test_alpha = 'Imported';
  setup.db.prepare(`INSERT INTO users
    (id,account_id,im_user_id,created_at,updated_at)
    VALUES('test_alpha','test_alpha','test_alpha',1,1)`).run();
  await assert.rejects(importCachedTestContacts(setup.db, setup.manifest, setup.options),
    /already has a MaiChat account/);
  assert.deepEqual(setup.imported, []);
  setup.db.close();
});

test('full test prefix preserves a 32-character MaiChat ID in the nickname', async () => {
  const setup = fixture();
  const id = `a${'b'.repeat(31)}`;
  setup.manifest.contacts = [{ im_user_id: id, expected_status: 'Imported' }];
  setup.options.imStatus = async () => 'Imported';
  await importCachedTestContacts(setup.db, setup.manifest, setup.options);
  assert.equal(setup.db.prepare('SELECT nickname FROM users WHERE id=?').get(id).nickname,
    `测试-${id}`);
  setup.db.close();
});
