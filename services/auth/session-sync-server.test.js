'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const { openSessionSyncStore } = require('./session-sync-store');
const { createSessionSyncService } = require('./session-sync-service');
const { createServer, createAccountAuthorizer } = require('./session-sync-server');

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
});
