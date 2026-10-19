'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const { openSessionSyncStore } = require('./session-sync-store');
const { createSessionSyncService } = require('./session-sync-service');
const { createServer, createAccountAuthorizer } = require('./session-sync-server');
const { openStore } = require('./store');
const { createAuthService } = require('./service');
const { createServer: createAuthServer } = require('./server');

test('HTTP sync adapter enforces body limits and authenticated device routing', async () => {
  const db = openSessionSyncStore(':memory:');
  const principals = new Map([
    ['mac-token', { accountId: 'alice', slot: 'mac', loginSessionId: 'mac-login-1' }],
    ['phone-token', { accountId: 'alice', slot: 'ios_phone', loginSessionId: 'phone-login' }]
  ]);
  const service = createSessionSyncService({ db,
    authorize: async headers => principals.get(headers?.authorization?.slice(7)) || null });
  const server = createServer(service);
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  try {
    const base = `http://127.0.0.1:${server.address().port}/v1/session-sync`;
    const send = (token, route, body) => fetch(`${base}/${route}`, { method: 'POST',
      headers: { Authorization: `Bearer ${token}`, 'Content-Type': 'application/json' },
      body: JSON.stringify(body) });
    assert.equal((await fetch(`${base}/sessions`)).status, 401);
    assert.equal((await send('mac-token', 'presence', { device_id: 'mac-install-1',
      name: 'Mac', sessions: [{ id: 'ses_1', title: 'Work', updated_at: 1 }] })).status, 200);
    assert.equal((await send('phone-token', 'presence', { device_id: 'phone-install-1',
      name: 'Phone', sessions: [] })).status, 200);
    const oversized = await fetch(`${base}/send`, { method: 'POST',
      body: 'x'.repeat(96 * 1024 + 1) });
    assert.equal(oversized.status, 413);
    const waiting = fetch(`${base}/poll?wait_ms=1000`, {
      headers: { Authorization: 'Bearer mac-token' }
    });
    const accepted = await send('phone-token', 'send', { to_slot: 'mac',
      to_login_session_id: 'mac-login-1', client_message_id: 'message-1',
      session_id: 'ses_1', kind: 'command', payload: { action: 'prompt', text: 'hello' } });
    assert.equal(accepted.status, 200);
    const result = await waiting;
    assert.equal(result.status, 200);
    assert.equal((await result.json()).messages[0].payload.text, 'hello');
  } finally {
    await new Promise(resolve => server.close(resolve));
    db.close();
  }
});

test('auth service response binds sync identity to account and login session', async () => {
  const calls = [];
  const authorize = createAccountAuthorizer('http://127.0.0.1:9100', async (url, init) => {
    calls.push({ url, authorization: init.headers.Authorization });
    return { status: 200, async json() { return { user: { id: 'alice' },
      device_slot: 'mac', login_session_id: 'mac-login-1' }; } };
  });
  assert.equal(await authorize({}), null);
  const token = 'a'.repeat(43);
  assert.deepEqual(await authorize({ authorization: `Bearer ${token}` }), {
    accountId: 'alice', slot: 'mac', loginSessionId: 'mac-login-1'
  });
  assert.deepEqual(calls, [{ url: 'http://127.0.0.1:9100/v1/auth/me',
    authorization: `Bearer ${token}` }]);
  const unavailable = createAccountAuthorizer('http://127.0.0.1:9100', async () => {
    throw new Error('Connection refused');
  });
  await assert.rejects(unavailable({ authorization: `Bearer ${token}` }),
    { code: 'AUTH_UNAVAILABLE' });
});

test('real MaiChat login controls which device can use the sync HTTP service', async () => {
  const authDb = openStore(':memory:');
  const syncDb = openSessionSyncStore(':memory:');
  let current = 1_780_000_000_000;
  let code = '';
  const auth = createAuthService({ db: authDb,
    sms: { async send(_phone, value) { code = value; } },
    secret: 'test-only-auth-hmac-key-keep-on-server-123456',
    imSecret: 'test-only-tencent-im-key-keep-on-server-123456',
    sdkAppId: 123456789, now: () => current });
  const authServer = createAuthServer(auth);
  await new Promise(resolve => authServer.listen(0, '127.0.0.1', resolve));
  const authOrigin = `http://127.0.0.1:${authServer.address().port}`;
  const sync = createSessionSyncService({ db: syncDb,
    authorize: createAccountAuthorizer(authOrigin) });
  const syncServer = createServer(sync);
  await new Promise(resolve => syncServer.listen(0, '127.0.0.1', resolve));
  const syncOrigin = `http://127.0.0.1:${syncServer.address().port}/v1/session-sync`;
  const signIn = async slot => {
    const challenge = await auth.handle({ method: 'POST', path: '/v1/auth/sms/challenge',
      body: { phone: '13900000001', age_confirmed_14_plus: true }, ip: '192.0.2.10' });
    assert.equal(challenge.statusCode, 200);
    const login = await auth.handle({ method: 'POST', path: '/v1/auth/sms/verify',
      body: { phone: '13900000001', challenge_id: challenge.body.challenge_id,
        code, age_confirmed_14_plus: true, device_slot: slot }, ip: '192.0.2.10' });
    assert.equal(login.statusCode, 200);
    return login.body.access_token;
  };
  const presence = (token, deviceId) => fetch(`${syncOrigin}/presence`, {
    method: 'POST', headers: { Authorization: `Bearer ${token}`,
      'Content-Type': 'application/json' },
    body: JSON.stringify({ device_id: deviceId, name: deviceId,
      sessions: [{ id: 'ses_1', title: 'Work', updated_at: 1 }] })
  });
  try {
    const oldToken = await signIn('ios_phone');
    assert.equal((await presence(oldToken, 'iphone-one')).status, 200);
    current += 60_001;
    const newToken = await signIn('ios_phone');
    assert.equal((await presence(oldToken, 'iphone-one')).status, 401);
    assert.equal((await presence(newToken, 'iphone-two')).status, 200);
    const listed = await fetch(`${syncOrigin}/sessions`, {
      headers: { Authorization: `Bearer ${newToken}` }
    });
    assert.equal(listed.status, 200);
    assert.equal((await listed.json()).sessions[0].device_id, 'iphone-two');
  } finally {
    await new Promise(resolve => syncServer.close(resolve));
    await new Promise(resolve => authServer.close(resolve));
    syncDb.close();
    authDb.close();
  }
});
