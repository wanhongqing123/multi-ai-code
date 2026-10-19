'use strict';

const assert = require('node:assert/strict');
const { mkdtempSync, rmSync } = require('node:fs');
const { tmpdir } = require('node:os');
const { join } = require('node:path');
const test = require('node:test');
const { openSessionSyncStore } = require('./session-sync-store');
const { createSessionSyncService } = require('./session-sync-service');

function fixture() {
  const db = openSessionSyncStore(':memory:');
  let current = 1_780_000_000_000;
  const principals = new Map([
    ['mac-token', { accountId: 'alice', slot: 'mac', loginSessionId: 'mac-login-1' }],
    ['mac-new-token', { accountId: 'alice', slot: 'mac', loginSessionId: 'mac-login-2' }],
    ['phone-token', { accountId: 'alice', slot: 'ios_phone', loginSessionId: 'phone-login' }],
    ['other-token', { accountId: 'bob', slot: 'ios_phone', loginSessionId: 'bob-login' }]
  ]);
  const service = createSessionSyncService({ db, now: () => current,
    authorize: async headers => principals.get(headers?.authorization?.slice(7)) || null });
  const call = (token, method, path, body = {}) => service.handle({
    method, path, body, headers: token ? { authorization: `Bearer ${token}` } : {}
  });
  const macSessions = [{ id: 'ses_mac_1', title: 'Build project', updated_at: current }];
  const macPresence = (token = 'mac-token') => call(token, 'POST',
    '/v1/session-sync/presence', { device_id: 'mac-install-1',
      name: 'Office Mac', sessions: macSessions });
  const phonePresence = () => call('phone-token', 'POST',
    '/v1/session-sync/presence', { device_id: 'phone-install-1',
      name: 'iPhone', sessions: [] });
  return { db, call, macPresence, phonePresence,
    advance(milliseconds) { current += milliseconds; } };
}

test('only the same account can list and address its active device sessions', async () => {
  const setup = fixture();
  try {
    assert.equal((await setup.call('', 'GET', '/v1/session-sync/sessions')).statusCode, 401);
    assert.equal((await setup.macPresence()).statusCode, 200);
    assert.equal((await setup.phonePresence()).statusCode, 200);
    const listed = await setup.call('phone-token', 'GET', '/v1/session-sync/sessions');
    assert.equal(listed.statusCode, 200);
    assert.equal(listed.body.sessions.length, 1);
    assert.equal(listed.body.sessions[0].title, 'Build project');
    assert.equal(listed.body.sessions[0].online, true);
    assert.deepEqual((await setup.call('other-token', 'GET',
      '/v1/session-sync/sessions')).body.sessions, []);
    assert.equal((await setup.call('other-token', 'POST', '/v1/session-sync/send', {
      to_slot: 'mac', to_login_session_id: 'mac-login-1',
      client_message_id: 'cross-account-command', session_id: 'ses_mac_1',
      kind: 'command', payload: { action: 'prompt', text: 'Not allowed' }
    })).statusCode, 409);

    const sent = await setup.call('phone-token', 'POST', '/v1/session-sync/send', {
      to_slot: 'mac', to_login_session_id: 'mac-login-1',
      client_message_id: 'command-1', session_id: 'ses_mac_1',
      kind: 'command', payload: { action: 'prompt', text: 'Continue building' }
    });
    assert.equal(sent.statusCode, 200);
    const duplicate = await setup.call('phone-token', 'POST', '/v1/session-sync/send', {
      to_slot: 'mac', to_login_session_id: 'mac-login-1',
      client_message_id: 'command-1', session_id: 'ses_mac_1',
      kind: 'command', payload: { action: 'prompt', text: 'Continue building' }
    });
    assert.equal(duplicate.body.id, sent.body.id);
    const received = await setup.call('mac-token', 'GET', '/v1/session-sync/poll');
    assert.equal(received.body.messages.length, 1);
    assert.equal(received.body.messages[0].payload.text, 'Continue building');
    assert.equal(received.body.messages[0].from_slot, 'ios_phone');
    assert.equal((await setup.call('mac-token', 'POST', '/v1/session-sync/ack',
      { ids: [sent.body.id] })).statusCode, 200);
    assert.deepEqual((await setup.call('mac-token', 'GET',
      '/v1/session-sync/poll')).body.messages, []);
    setup.advance(61_000);
    const afterAck = await setup.call('phone-token', 'POST', '/v1/session-sync/send', {
      to_slot: 'mac', to_login_session_id: 'mac-login-1',
      client_message_id: 'command-1', session_id: 'ses_mac_1',
      kind: 'command', payload: { action: 'prompt', text: 'Continue building' }
    });
    assert.deepEqual(afterAck.body, { id: sent.body.id, duplicate: true });
    const changed = await setup.call('phone-token', 'POST', '/v1/session-sync/send', {
      to_slot: 'mac', to_login_session_id: 'mac-login-1',
      client_message_id: 'command-1', session_id: 'ses_mac_1',
      kind: 'command', payload: { action: 'prompt', text: 'Run a paid task' }
    });
    assert.equal(changed.statusCode, 409);
  } finally { setup.db.close(); }
});

test('offline and replaced device logins cannot receive stale commands', async () => {
  const setup = fixture();
  try {
    await setup.macPresence();
    await setup.phonePresence();
    setup.advance(46_000);
    const offline = await setup.call('phone-token', 'POST', '/v1/session-sync/send', {
      to_slot: 'mac', to_login_session_id: 'mac-login-1',
      client_message_id: 'offline-command', session_id: 'ses_mac_1',
      kind: 'command', payload: { action: 'prompt', text: 'Do not queue me' }
    });
    assert.equal(offline.statusCode, 409);
    const sessions = await setup.call('phone-token', 'GET', '/v1/session-sync/sessions');
    assert.equal(sessions.body.sessions[0].online, false);

    assert.equal((await setup.macPresence('mac-new-token')).statusCode, 200);
    assert.equal((await setup.call('mac-token', 'GET', '/v1/session-sync/poll')).statusCode,
      409);
    const stale = await setup.call('phone-token', 'POST', '/v1/session-sync/send', {
      to_slot: 'mac', to_login_session_id: 'mac-login-1',
      client_message_id: 'stale-command', session_id: 'ses_mac_1',
      kind: 'command', payload: { action: 'prompt', text: 'Wrong machine' }
    });
    assert.equal(stale.statusCode, 409);
    assert.deepEqual((await setup.call('mac-new-token', 'GET',
      '/v1/session-sync/poll')).body.messages, []);
  } finally { setup.db.close(); }
});

test('a waiting poll wakes when a command arrives', async () => {
  const setup = fixture();
  try {
    await setup.macPresence();
    await setup.phonePresence();
    const waiting = setup.call('mac-token', 'GET', '/v1/session-sync/poll?wait_ms=1000');
    const sent = await setup.call('phone-token', 'POST', '/v1/session-sync/send', {
      to_slot: 'mac', to_login_session_id: 'mac-login-1',
      client_message_id: 'wake-command', session_id: 'ses_mac_1',
      kind: 'history_request', payload: { after: '' }
    });
    assert.equal(sent.statusCode, 200);
    assert.equal((await waiting).body.messages.length, 1);
  } finally { setup.db.close(); }
});

test('Session directory and pending envelopes survive a service restart', async () => {
  const directory = mkdtempSync(join(tmpdir(), 'maichat-session-sync-'));
  const filename = join(directory, 'sync.db');
  let db = openSessionSyncStore(filename);
  const authorize = async headers => headers?.authorization === 'Bearer mac-token'
    ? { accountId: 'alice', slot: 'mac', loginSessionId: 'mac-login-1' }
    : { accountId: 'alice', slot: 'ios_phone', loginSessionId: 'phone-login' };
  let service = createSessionSyncService({ db, authorize });
  const call = (token, method, path, body = {}) => service.handle({
    method, path, body, headers: { authorization: `Bearer ${token}` }
  });
  try {
    await call('mac-token', 'POST', '/v1/session-sync/presence', {
      device_id: 'mac-install-1', name: 'Mac',
      sessions: [{ id: 'ses_1', title: 'A project', updated_at: Date.now() }]
    });
    await call('phone-token', 'POST', '/v1/session-sync/presence', {
      device_id: 'phone-install-1', name: 'iPhone', sessions: []
    });
    const sent = await call('phone-token', 'POST', '/v1/session-sync/send', {
      to_slot: 'mac', to_login_session_id: 'mac-login-1',
      client_message_id: 'saved-command', session_id: 'ses_1',
      kind: 'command', payload: { action: 'prompt', text: 'Check the build' }
    });
    assert.equal(sent.statusCode, 200);
    db.close();
    db = openSessionSyncStore(filename);
    service = createSessionSyncService({ db, authorize });
    const sessions = await call('phone-token', 'GET', '/v1/session-sync/sessions');
    assert.equal(sessions.body.sessions[0].title, 'A project');
    assert.equal(sessions.body.sessions[0].online, false);
    const received = await call('mac-token', 'GET', '/v1/session-sync/poll');
    assert.equal(received.body.messages[0].payload.text, 'Check the build');
  } finally {
    if (db.open) db.close();
    rmSync(directory, { recursive: true, force: true });
  }
});
