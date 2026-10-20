'use strict';

const assert = require('node:assert/strict');
const { randomBytes } = require('node:crypto');
const test = require('node:test');
const { openStore } = require('./store');
const { createAuthService } = require('./service');
const { createAdminServer } = require('./admin-server');

test('local admin page requires authentication and same-origin POST', async () => {
  const db = openStore(':memory:');
  const service = createAuthService({ db, sms: { async send() {} },
    secret: randomBytes(32).toString('hex'), imSecret: randomBytes(32).toString('hex'),
    sdkAppId: 123456789 });
  const server = createAdminServer({ db, service, secret: 'admin-test-secret-at-least-32-chars' });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  try {
    const base = `http://127.0.0.1:${server.address().port}`;
    const authorization = 'Basic ' + Buffer.from(
      'admin:admin-test-secret-at-least-32-chars').toString('base64');
    assert.equal((await fetch(base)).status, 401);
    assert.equal((await fetch(base, { headers: { authorization } })).status, 200);
    const body = new URLSearchParams({ account_id: 'test_admin',
      password: 'admin-password-123456789' });
    assert.equal((await fetch(`${base}/create`, { method: 'POST',
      headers: { authorization, 'content-type': 'application/x-www-form-urlencoded',
        origin: 'https://example.invalid' }, body })).status, 403);
    assert.equal(db.prepare('SELECT COUNT(*) AS count FROM users').get().count, 0);
    assert.equal((await fetch(`${base}/create`, { method: 'POST',
      headers: { authorization, 'content-type': 'application/x-www-form-urlencoded',
        origin: base }, body })).status, 200);
    assert.equal(db.prepare('SELECT phone_e164,im_user_id FROM users WHERE id=?')
      .get('test_admin').im_user_id, 'test_admin');
    const html = await (await fetch(base, { headers: { authorization } })).text();
    assert.equal(html.includes('admin-password-123456789'), false);
    assert.equal((await service.createTestAccount('report_target',
      'report-target-password-123456')).statusCode, 201);
    const login = await service.handle({ method: 'POST', path: '/v1/auth/password/login',
      body: { account_id: 'test_admin', password: 'admin-password-123456789',
        age_confirmed_14_plus: true, device_slot: 'ios_phone' },
      ip: '192.0.2.1', headers: {} });
    assert.equal(login.statusCode, 200);
    const reported = await service.handle({ method: 'POST', path: '/v1/safety/reports',
      body: { target_im_user_id: 'report_target', reason: 'spam' },
      ip: '192.0.2.1',
      headers: { authorization: `Bearer ${login.body.access_token}` } });
    assert.equal(reported.statusCode, 201);
    const pending = await (await fetch(base, { headers: { authorization } })).text();
    assert.match(pending, /report_target/);
    const resolution = new URLSearchParams({ report_id: reported.body.report_id,
      resolution: 'Reviewed and action taken' });
    assert.equal((await fetch(`${base}/resolve-report`, { method: 'POST',
      headers: { authorization, 'content-type': 'application/x-www-form-urlencoded',
        origin: base }, body: resolution })).status, 200);
    const own = await service.handle({ method: 'GET', path: '/v1/safety/reports',
      body: {}, ip: '192.0.2.1',
      headers: { authorization: `Bearer ${login.body.access_token}` } });
    assert.equal(own.body.reports[0].status, 'resolved');
  } finally {
    await new Promise(resolve => server.close(resolve));
    db.close();
  }
});
