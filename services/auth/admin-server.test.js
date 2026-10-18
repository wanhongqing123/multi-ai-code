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
  } finally {
    await new Promise(resolve => server.close(resolve));
    db.close();
  }
});
