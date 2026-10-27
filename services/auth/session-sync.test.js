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
    ['windows-token', { accountId: 'alice', slot: 'windows', loginSessionId: 'windows-login' }],
    ['other-token', { accountId: 'bob', slot: 'ios_phone', loginSessionId: 'bob-login' }]
  ]);
  const service = createSessionSyncService({ db, now: () => current,
    authorize: async headers => principals.get(headers?.authorization?.slice(7)) || null });
  const call = (token, method, path, body = {}) => service.handle({
    method, path, body, headers: token ? { authorization: `Bearer ${token}` } : {}
  });
  const macSessions = [{ id: 'ses_mac_1', title: 'Build project', updated_at: current,
    busy: true }];
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
    assert.equal(listed.body.sessions[0].busy, true);
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
    assert.deepEqual((await setup.call('mac-token', 'GET',
      '/v1/session-sync/poll')).body.messages, []);
    assert.equal((await setup.call('mac-token', 'POST', '/v1/session-sync/ack',
      { ids: [sent.body.id] })).statusCode, 200);
    assert.deepEqual((await setup.call('mac-token', 'GET',
      '/v1/session-sync/poll')).body.messages, []);
    const event = await setup.call('mac-token', 'POST', '/v1/session-sync/send', {
      to_slot: 'ios_phone', to_login_session_id: 'phone-login',
      client_message_id: 'event-1', session_id: 'ses_mac_1',
      kind: 'event', payload: { message_id: 'msg_1', text: 'Build completed' }
    });
    assert.equal(event.statusCode, 200);
    assert.equal((await setup.call('phone-token', 'GET',
      '/v1/session-sync/poll')).body.messages.length, 1);
    assert.equal((await setup.call('phone-token', 'GET',
      '/v1/session-sync/poll')).body.messages.length, 0);
    setup.advance(30_001);
    assert.equal((await setup.call('phone-token', 'GET',
      '/v1/session-sync/poll')).body.messages.length, 1);
    await setup.call('phone-token', 'POST', '/v1/session-sync/ack',
      { ids: [event.body.id] });
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

test('later owner snapshots retain durable Session attachment references', async () => {
  const setup = fixture();
  try {
    await setup.macPresence();
    await setup.phonePresence();
    const base = { id:'msg_attached',role:'user',created:1,completed:1,active:false,
      parts:[{ id:'image_part',kind:'image',mimeType:'image/jpeg' }] };
    const first = { ...base,parts:[
      { ...base.parts[0],attachment_id:'123e4567-e89b-42d3-a456-426614174000',
        name:'photo.jpg' },
      { id:'session_attachment_file',kind:'file',name:'notes.pdf',
        attachment_id:'123e4567-e89b-42d3-a456-426614174001' }
    ] };
    assert.equal((await setup.call('mac-token','POST','/v1/session-sync/publish',
      { session_id:'ses_mac_1',messages:[first] })).statusCode,200);
    assert.equal((await setup.call('mac-token','POST','/v1/session-sync/publish',
      { session_id:'ses_mac_1',messages:[base] })).statusCode,200);
    const history = await setup.call('phone-token','GET',
      '/v1/session-sync/history?owner_slot=mac&session_id=ses_mac_1');
    assert.equal(history.statusCode,200);
    assert.deepEqual(history.body.messages[0].parts.map(part => part.attachment_id),
      ['123e4567-e89b-42d3-a456-426614174000',
       '123e4567-e89b-42d3-a456-426614174001']);
  } finally { setup.db.close(); }
});

test('retaining an attachment never silently drops an oversized owner snapshot', async () => {
  const setup = fixture();
  try {
    await setup.macPresence();
    const attachment = { id:'session_attachment_file',kind:'file',name:'notes.pdf',
      attachment_id:'123e4567-e89b-42d3-a456-426614174001' };
    const original = { id:'msg_attached',role:'user',created:1,
      parts:[attachment] };
    assert.equal((await setup.call('mac-token','POST','/v1/session-sync/publish',
      { session_id:'ses_mac_1',messages:[original] })).statusCode,200);
    const oversizedAfterMerge = { ...original,parts:[
      { id:'text_part',kind:'text',text:'x'.repeat(48 * 1024 - 200) }] };
    assert.equal((await setup.call('mac-token','POST','/v1/session-sync/publish',
      { session_id:'ses_mac_1',messages:[oversizedAfterMerge] })).statusCode,413);
    const history = await setup.call('mac-token','GET',
      '/v1/session-sync/history?owner_slot=mac&session_id=ses_mac_1');
    assert.deepEqual(history.body.messages[0].parts,[attachment]);
  } finally { setup.db.close(); }
});

test('one account routes a Session reply independently to phone and Windows members', async () => {
  const setup = fixture();
  try {
    await setup.macPresence();
    await setup.phonePresence();
    assert.equal((await setup.call('windows-token', 'POST', '/v1/session-sync/presence', {
      device_id: 'windows-install-1', name: 'Windows', sessions: []
    })).statusCode, 200);
    for (const [token, id] of [['phone-token', 'phone-request'],
      ['windows-token', 'windows-request']]) {
      assert.equal((await setup.call(token, 'POST', '/v1/session-sync/send', {
        to_slot: 'mac', to_login_session_id: 'mac-login-1',
        client_message_id: id, session_id: 'ses_mac_1', kind: 'command',
        payload: { action: 'prompt', text: '你好' }
      })).statusCode, 200);
    }
    const requests = (await setup.call('mac-token', 'GET', '/v1/session-sync/poll')).body.messages;
    assert.deepEqual(requests.map(item => item.from_slot), ['ios_phone', 'windows']);
    for (const [slot, login] of [['ios_phone', 'phone-login'], ['windows', 'windows-login']]) {
      assert.equal((await setup.call('mac-token', 'POST', '/v1/session-sync/send', {
        to_slot: slot, to_login_session_id: login,
        client_message_id: `reply-${slot}`, session_id: 'ses_mac_1', kind: 'event',
        payload: { message_id: 'msg_reply', text: '收到' }
      })).statusCode, 200);
    }
    const phone = (await setup.call('phone-token', 'GET', '/v1/session-sync/poll')).body.messages;
    const windows = (await setup.call('windows-token', 'GET', '/v1/session-sync/poll')).body.messages;
    assert.equal(phone.length, 1);
    assert.equal(windows.length, 1);
    assert.equal(phone[0].payload.message_id, 'msg_reply');
    assert.equal(windows[0].payload.message_id, 'msg_reply');
    await setup.call('phone-token', 'POST', '/v1/session-sync/ack', { ids: [phone[0].id] });
    assert.equal((await setup.call('windows-token', 'GET', '/v1/session-sync/poll'))
      .body.messages.length, 0);
  } finally { setup.db.close(); }
});

test('a same-account device can request creation on an owner with no Sessions', async () => {
  const setup = fixture();
  try {
    assert.equal((await setup.call('mac-token', 'POST', '/v1/session-sync/presence', {
      device_id: 'mac-install-1', name: 'Office Mac', sessions: []
    })).statusCode, 200);
    await setup.phonePresence();
    const catalog = await setup.call('phone-token', 'GET', '/v1/session-sync/sessions');
    assert.deepEqual(catalog.body.sessions, []);
    assert.equal(catalog.body.devices.find(item => item.device_slot === 'mac')?.online,
      true);
    assert.equal((await setup.call('phone-token', 'POST', '/v1/session-sync/send', {
      to_slot: 'mac', to_login_session_id: 'mac-login-1',
      client_message_id: 'create-relative', session_id: 'session_lifecycle',
      kind: 'command', payload: { action: 'session_create',
        workspace_path: 'relative/path' }
    })).statusCode, 400);

    const command = await setup.call('phone-token', 'POST', '/v1/session-sync/send', {
      to_slot: 'mac', to_login_session_id: 'mac-login-1',
      client_message_id: 'create-command', session_id: 'session_lifecycle',
      kind: 'command', payload: { action: 'session_create' }
    });
    assert.equal(command.statusCode, 200);
    const received = (await setup.call('mac-token', 'GET',
      '/v1/session-sync/poll')).body.messages;
    assert.equal(received.length, 1);
    assert.equal(received[0].client_message_id, 'create-command');
    assert.deepEqual(received[0].payload, { action: 'session_create' });
    assert.equal((await setup.call('mac-token', 'POST', '/v1/session-sync/ack',
      { ids: [received[0].id] })).statusCode, 200);

    assert.equal((await setup.call('mac-token', 'POST', '/v1/session-sync/presence', {
      device_id: 'mac-install-1', name: 'Office Mac',
      sessions: [{ id: 'created_1', title: 'New Session', updated_at: 1 }]
    })).statusCode, 200);
    assert.equal((await setup.call('mac-token', 'POST', '/v1/session-sync/send', {
      to_slot: 'ios_phone', to_login_session_id: 'phone-login',
      client_message_id: 'create-bad-result', session_id: 'session_lifecycle',
      kind: 'event', payload: { type: 'command_result', command_id: 'create-command',
        result: { ok: true } }
    })).statusCode, 400);
    const result = await setup.call('mac-token', 'POST', '/v1/session-sync/send', {
      to_slot: 'ios_phone', to_login_session_id: 'phone-login',
      client_message_id: 'create-result', session_id: 'session_lifecycle',
      kind: 'event', payload: { type: 'command_result', command_id: 'create-command',
        result: { ok: true, session_id: 'created_1' } }
    });
    assert.equal(result.statusCode, 200);
    const reply = (await setup.call('phone-token', 'GET',
      '/v1/session-sync/poll')).body.messages;
    assert.equal(reply.length, 1);
    assert.equal(reply[0].payload.command_id, 'create-command');
    assert.equal(reply[0].payload.result.session_id, 'created_1');
    assert.equal((await setup.call('phone-token', 'GET', '/v1/session-sync/sessions'))
      .body.sessions[0].id, 'created_1');
  } finally { setup.db.close(); }
});

test('remote delete requires an existing owned Session and correlated result', async () => {
  const setup = fixture();
  try {
    await setup.macPresence();
    await setup.phonePresence();
    const wrong = await setup.call('phone-token', 'POST', '/v1/session-sync/send', {
      to_slot: 'mac', to_login_session_id: 'mac-login-1',
      client_message_id: 'delete-missing', session_id: 'session_lifecycle',
      kind: 'command', payload: { action: 'session_delete',
        target_session_id: 'missing' }
    });
    assert.equal(wrong.statusCode, 404);
    const sent = await setup.call('phone-token', 'POST', '/v1/session-sync/send', {
      to_slot: 'mac', to_login_session_id: 'mac-login-1',
      client_message_id: 'delete-command', session_id: 'session_lifecycle',
      kind: 'command', payload: { action: 'session_delete',
        target_session_id: 'ses_mac_1' }
    });
    assert.equal(sent.statusCode, 200);
    assert.equal((await setup.call('mac-token', 'GET', '/v1/session-sync/poll'))
      .body.messages[0].payload.target_session_id, 'ses_mac_1');
    const forged = await setup.call('mac-token', 'POST', '/v1/session-sync/send', {
      to_slot: 'ios_phone', to_login_session_id: 'phone-login',
      client_message_id: 'forged-result', session_id: 'session_lifecycle',
      kind: 'event', payload: { type: 'command_result', command_id: 'not-a-command',
        result: { ok: true, session_id: 'ses_mac_1' } }
    });
    assert.equal(forged.statusCode, 404);
    assert.equal((await setup.call('mac-token', 'POST', '/v1/session-sync/presence', {
      device_id: 'mac-install-1', name: 'Office Mac', sessions: []
    })).statusCode, 200);
    const result = await setup.call('mac-token', 'POST', '/v1/session-sync/send', {
      to_slot: 'ios_phone', to_login_session_id: 'phone-login',
      client_message_id: 'delete-result', session_id: 'session_lifecycle',
      kind: 'event', payload: { type: 'command_result', command_id: 'delete-command',
        result: { ok: true, session_id: 'ses_mac_1' } }
    });
    assert.equal(result.statusCode, 200);
    assert.deepEqual((await setup.call('phone-token', 'GET',
      '/v1/session-sync/sessions')).body.sessions, []);
  } finally { setup.db.close(); }
});

test('Workspace switch routes to the owner of an existing Session', async () => {
  const setup = fixture();
  try {
    await setup.macPresence();
    await setup.phonePresence();
    const base = { to_slot: 'mac', to_login_session_id: 'mac-login-1',
      session_id: 'session_lifecycle', kind: 'command',
      payload: { action: 'session_set_workspace',
        target_session_id: 'ses_mac_1', workspace_path: '/tmp/maichat-project' } };
    for (const [id, payload, expected] of [
      ['switch-no-path', { action: 'session_set_workspace',
        target_session_id: 'ses_mac_1' }, 400],
      ['switch-relative', { ...base.payload, workspace_path: 'relative/project' }, 400],
      ['switch-missing', { ...base.payload, target_session_id: 'ses_missing' }, 404]
    ]) {
      assert.equal((await setup.call('phone-token', 'POST', '/v1/session-sync/send', {
        ...base, client_message_id: id, payload
      })).statusCode, expected);
    }
    const sent = await setup.call('phone-token', 'POST', '/v1/session-sync/send', {
      ...base, client_message_id: 'switch-valid'
    });
    assert.equal(sent.statusCode, 200);
    const received = (await setup.call('mac-token', 'GET',
      '/v1/session-sync/poll')).body.messages;
    assert.equal(received.length, 1);
    assert.deepEqual(received[0].payload, base.payload);
    assert.equal(received[0].client_message_id, 'switch-valid');
    assert.equal((await setup.call('mac-token', 'POST', '/v1/session-sync/send', {
      to_slot: 'ios_phone', to_login_session_id: 'phone-login',
      client_message_id: 'switch-busy-result', session_id: 'session_lifecycle',
      kind: 'event', payload: { type: 'command_result', command_id: 'switch-valid',
        result: { ok: false, error: 'Session is busy' } }
    })).statusCode, 200);
    assert.equal((await setup.call('phone-token', 'GET',
      '/v1/session-sync/poll')).body.messages[0].payload.result.error,
    'Session is busy');
    assert.equal((await setup.call('phone-token', 'GET',
      '/v1/session-sync/sessions')).body.sessions[0].id, 'ses_mac_1');
  } finally { setup.db.close(); }
});

test('Session lifecycle commands reject offline and replaced logins without replay', async () => {
  const setup = fixture();
  try {
    await setup.macPresence();
    await setup.phonePresence();
    const create = { to_slot: 'mac', to_login_session_id: 'mac-login-1',
      client_message_id: 'lifecycle-offline', session_id: 'session_lifecycle',
      kind: 'command', payload: { action: 'session_create' } };
    const workspaceSwitch = { ...create,
      client_message_id: 'workspace-offline', payload: {
        action: 'session_set_workspace', target_session_id: 'ses_mac_1',
        workspace_path: '/tmp/maichat-project' } };
    setup.advance(46_000);
    assert.equal((await setup.call('phone-token', 'POST', '/v1/session-sync/send',
      create)).statusCode, 409);
    assert.equal((await setup.call('phone-token', 'POST', '/v1/session-sync/send',
      workspaceSwitch)).statusCode, 409);
    assert.equal((await setup.macPresence('mac-new-token')).statusCode, 200);
    assert.equal((await setup.call('phone-token', 'POST', '/v1/session-sync/send',
      { ...create, client_message_id: 'lifecycle-stale' })).statusCode, 409);
    assert.equal((await setup.call('phone-token', 'POST', '/v1/session-sync/send',
      { ...workspaceSwitch, client_message_id: 'workspace-stale' })).statusCode, 409);
    assert.equal((await setup.call('mac-token', 'GET',
      '/v1/session-sync/poll')).statusCode, 409);
    const newCommand = { ...create, to_login_session_id: 'mac-login-2',
      client_message_id: 'lifecycle-current' };
    assert.equal((await setup.call('phone-token', 'POST', '/v1/session-sync/send',
      newCommand)).statusCode, 200);
    assert.equal((await setup.call('phone-token', 'POST', '/v1/session-sync/send',
      { ...workspaceSwitch, to_login_session_id: 'mac-login-2',
        client_message_id: 'workspace-current' })).statusCode, 200);
    const delivered = (await setup.call('mac-new-token', 'GET',
      '/v1/session-sync/poll')).body.messages;
    assert.equal(delivered.length, 2);
    assert.deepEqual(delivered.map(message => message.payload.action),
      ['session_create', 'session_set_workspace']);
    assert.equal((await setup.macPresence('mac-new-token')).statusCode, 200);
    assert.deepEqual((await setup.call('mac-new-token', 'GET',
      '/v1/session-sync/poll')).body.messages, []);
  } finally { setup.db.close(); }
});

test('owner publishes one canonical room log for all device slots with shared paging', async () => {
  const setup = fixture();
  try {
    await setup.macPresence();
    await setup.phonePresence();
    await setup.call('windows-token', 'POST', '/v1/session-sync/presence', {
      device_id: 'windows-install-1', name: 'Windows', sessions: []
    });
    const user = { id: 'msg_001', role: 'user', created: 1,
      parts: [{ id: 'part_001', kind: 'text', text: '你好' }] };
    const assistant = { id: 'msg_002', role: 'assistant', created: 2,
      parts: [{ id: 'part_002', kind: 'text', text: '收到' }] };
    const first = await setup.call('mac-token', 'POST', '/v1/session-sync/publish', {
      session_id: 'ses_mac_1', messages: [user, assistant]
    });
    assert.deepEqual(first.body, { published: 2 });
    assert.deepEqual((await setup.call('mac-token', 'POST', '/v1/session-sync/publish', {
      session_id: 'ses_mac_1', messages: [user, assistant]
    })).body, { published: 0 });
    for (const token of ['phone-token', 'windows-token']) {
      const page = await setup.call(token, 'GET',
        '/v1/session-sync/history?owner_slot=mac&session_id=ses_mac_1&limit=1');
      assert.deepEqual(page.body.messages, [assistant]);
      const older = await setup.call(token, 'GET',
        '/v1/session-sync/history?owner_slot=mac&session_id=ses_mac_1&before=msg_002&limit=20');
      assert.deepEqual(older.body.messages, [user]);
      const inbox = (await setup.call(token, 'GET', '/v1/session-sync/poll')).body.messages;
      assert.equal(inbox.length, 2);
      assert.equal(inbox.every(item => item.kind === 'event'), true);
    }
    assert.equal((await setup.call('phone-token', 'POST', '/v1/session-sync/publish', {
      session_id: 'ses_mac_1', messages: [user]
    })).statusCode, 404);
    assert.equal((await setup.call('mac-token', 'POST', '/v1/session-sync/publish', {
      session_id: 'ses_mac_1', messages: [{ id: 'msg_oversized', role: 'assistant',
        parts: [{ kind: 'text', text: 'x'.repeat(49 * 1024) }] }]
    })).statusCode, 400);
    assert.equal((await setup.call('other-token', 'GET',
      '/v1/session-sync/history?owner_slot=mac&session_id=ses_mac_1')).statusCode, 404);
    assert.equal((await setup.call('phone-token', 'POST', '/v1/session-sync/room-delete',
      { session_id: 'ses_mac_1' })).statusCode, 200);
    assert.equal((await setup.call('mac-token', 'POST', '/v1/session-sync/room-delete',
      { session_id: 'ses_mac_1' })).body.removed, 2);
    assert.deepEqual((await setup.call('windows-token', 'GET',
      '/v1/session-sync/history?owner_slot=mac&session_id=ses_mac_1'))
      .body.messages, []);
  } finally { setup.db.close(); }
});

test('shared room history pages past the newest twenty messages without gaps', async () => {
  const setup = fixture();
  try {
    await setup.macPresence();
    const entries = Array.from({ length: 45 }, (_, index) => ({
      id: `msg_${String(index + 1).padStart(3, '0')}`, role: 'assistant',
      parts: [{ kind: 'text', text: `reply ${index + 1}` }]
    }));
    for (let index = 0; index < entries.length; index += 10) {
      const published = await setup.call('mac-token', 'POST', '/v1/session-sync/publish', {
        session_id: 'ses_mac_1', messages: entries.slice(index, index + 10),
        permissions: [], questions: []
      });
      assert.equal(published.statusCode, 200);
    }
    const ids = [];
    let before = '';
    for (const size of [20, 20, 5]) {
      const page = await setup.call('phone-token', 'GET',
        `/v1/session-sync/history?owner_slot=mac&session_id=ses_mac_1&limit=20&before=${before}`);
      assert.equal(page.body.messages.length, size);
      ids.push(...page.body.messages.map(message => message.id));
      before = page.body.messages.at(-1).id;
    }
    assert.deepEqual(ids, entries.map(message => message.id).reverse());
  } finally { setup.db.close(); }
});

test('acknowledging the first inbox batch exposes the remaining room updates', async () => {
  const setup = fixture();
  try {
    await setup.macPresence();
    await setup.phonePresence();
    const messages = Array.from({ length: 12 }, (_, index) => ({
      id: `msg_batch_${index}`, role: 'assistant',
      parts: [{ kind: 'text', text: `reply ${index}` }]
    }));
    assert.equal((await setup.call('mac-token', 'POST', '/v1/session-sync/publish', {
      session_id: 'ses_mac_1', messages, permissions: [], questions: []
    })).statusCode, 200);
    const first = (await setup.call('phone-token', 'GET', '/v1/session-sync/poll')).body.messages;
    assert.equal(first.length, 8);
    const second = (await setup.call('phone-token', 'GET', '/v1/session-sync/poll')).body.messages;
    assert.equal(second.length, 4);
    assert.equal(new Set([...first, ...second].map(message => message.id)).size, 12);
    assert.deepEqual((await setup.call('phone-token', 'GET',
      '/v1/session-sync/poll')).body.messages, []);
    await setup.call('phone-token', 'POST', '/v1/session-sync/ack',
      { ids: first.map(message => message.id) });
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
      kind: 'command', payload: { action: 'prompt', text: '你好' }
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
    assert.equal((await call('mac-token', 'POST', '/v1/session-sync/publish', {
      session_id: 'ses_1', messages: [{ id: 'msg_saved', role: 'assistant',
        parts: [{ kind: 'text', text: 'Persisted reply' }] }],
      permissions: [], questions: []
    })).statusCode, 200);
    db.close();
    db = openSessionSyncStore(filename);
    service = createSessionSyncService({ db, authorize });
    const sessions = await call('phone-token', 'GET', '/v1/session-sync/sessions');
    assert.equal(sessions.body.sessions[0].title, 'A project');
    assert.equal(sessions.body.sessions[0].online, false);
    const history = await call('phone-token', 'GET',
      '/v1/session-sync/history?owner_slot=mac&session_id=ses_1&limit=10');
    assert.equal(history.body.messages[0].parts[0].text, 'Persisted reply');
    const received = await call('mac-token', 'GET', '/v1/session-sync/poll');
    assert.equal(received.body.messages[0].payload.text, 'Check the build');
  } finally {
    if (db.open) db.close();
    rmSync(directory, { recursive: true, force: true });
  }
});
