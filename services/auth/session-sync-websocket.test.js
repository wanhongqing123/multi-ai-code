'use strict';

const assert = require('node:assert/strict');
const { after, test } = require('node:test');
const { once } = require('node:events');
const { WebSocket } = require('ws');
const { openSessionSyncStore } = require('./session-sync-store');
const { createSessionSyncService } = require('./session-sync-service');
const { createServer } = require('./session-sync-server');

function connected(url, token) {
  const socket = new WebSocket(url, { headers: { Authorization: `Bearer ${token}` } });
  const received = [];
  const waiters = [];
  socket.on('message', bytes => {
    const frame = JSON.parse(bytes.toString());
    const index = waiters.findIndex(waiter => waiter.matches(frame));
    if (index < 0) received.push(frame);
    else waiters.splice(index, 1)[0].resolve(frame);
  });
  function next(matches) {
    const index = received.findIndex(matches);
    if (index >= 0) return Promise.resolve(received.splice(index, 1)[0]);
    return new Promise((resolve, reject) => {
      const waiter = { matches, resolve: frame => { clearTimeout(timer); resolve(frame); } };
      const timer = setTimeout(() => {
        const active = waiters.indexOf(waiter);
        if (active >= 0) waiters.splice(active, 1);
        reject(new Error('Timed out waiting for WebSocket frame'));
      }, 3000);
      waiters.push(waiter);
    });
  }
  function request(id, op, body = {}) {
    socket.send(JSON.stringify({ id, op, body }));
    return next(frame => frame.type === 'response' && frame.id === id);
  }
  return { socket, next, request };
}

test('WebSocket pushes same-account catalog and mailbox without HTTP polling', async () => {
  const db = openSessionSyncStore(':memory:');
  const principals = new Map([
    ['mac-token', { accountId: 'alice', slot: 'mac', loginSessionId: 'mac-login' }],
    ['phone-token', { accountId: 'alice', slot: 'ios_phone', loginSessionId: 'phone-login' }],
    ['other-token', { accountId: 'bob', slot: 'ios_phone', loginSessionId: 'other-login' }]
  ]);
  const authorize = async headers => principals.get(headers.authorization?.slice(7)) || null;
  const service = createSessionSyncService({ db, authorize });
  const server = createServer(service, authorize);
  server.listen(0, '127.0.0.1');
  await once(server, 'listening');
  const url = `ws://127.0.0.1:${server.address().port}/v1/session-sync/ws`;
  const mac = connected(url, 'mac-token');
  const phone = connected(url, 'phone-token');
  const other = connected(url, 'other-token');
  let renewed;
  try {
    await Promise.all([once(mac.socket, 'open'), once(phone.socket, 'open'),
      once(other.socket, 'open')]);
    assert.equal((await phone.request('not-present', 'catalog')).status, 409);
    const macPresence = await mac.request('mac-presence', 'presence', {
      device_id: 'mac-install', name: 'Work Mac',
      sessions: [{ id: 'ses_mac', title: 'Build', updated_at: Date.now() }]
    });
    assert.equal(macPresence.status, 200);
    assert.equal((await phone.request('phone-presence', 'presence', {
      device_id: 'phone-install', name: 'iPhone', sessions: []
    })).status, 200);
    assert.equal((await other.request('other-presence', 'presence', {
      device_id: 'other-install', name: 'Other', sessions: []
    })).status, 200);
    const catalog = await phone.next(frame => frame.type === 'catalog' &&
      frame.sessions.some(session => session.id === 'ses_mac'));
    assert.equal(catalog.sessions[0].online, true);
    assert.equal((await mac.request('mac-update', 'presence', {
      device_id: 'mac-install', name: 'Work Mac',
      sessions: [{ id: 'ses_mac', title: 'Build', updated_at: Date.now() },
        { id: 'ses_new', title: 'New Session', updated_at: Date.now() }]
    })).status, 200);
    const pushed = await phone.next(frame => frame.type === 'catalog' &&
      frame.sessions.some(session => session.id === 'ses_new'));
    assert.equal(pushed.sessions.length, 2);
    const isolated = await other.request('other-catalog', 'catalog');
    assert.deepEqual(isolated.body.sessions, []);

    const body = { to_slot: 'mac', to_login_session_id: 'mac-login',
      client_message_id: 'request-one', session_id: 'ses_mac',
      kind: 'history_request', payload: { limit: 1 } };
    const first = await phone.request('send-one', 'send', body);
    assert.equal(first.status, 200);
    const delivered = await mac.next(frame => frame.type === 'inbox' &&
      frame.messages.some(message => message.client_message_id === 'request-one'));
    assert.equal(delivered.messages.length, 1);
    const duplicate = await phone.request('send-two', 'send', body);
    assert.equal(duplicate.status, 200);
    assert.equal(duplicate.body.duplicate, true);
    assert.equal(duplicate.body.id, first.body.id);
    assert.equal((await mac.request('ack-one', 'ack', {
      ids: [delivered.messages[0].id]
    })).status, 200);

    const retryBody = { ...body, client_message_id: 'request-reconnect' };
    assert.equal((await phone.request('send-reconnect', 'send', retryBody)).status, 200);
    await mac.next(frame => frame.type === 'inbox' &&
      frame.messages.some(message => message.client_message_id === 'request-reconnect'));

    mac.socket.close();
    await once(mac.socket, 'close');
    const offline = await phone.next(frame => frame.type === 'catalog' &&
      frame.sessions.some(session => session.id === 'ses_mac' && !session.online));
    assert.equal(offline.sessions[0].online, false);
    renewed = connected(url, 'mac-token');
    await once(renewed.socket, 'open');
    assert.equal((await renewed.request('mac-return', 'presence', {
      device_id: 'mac-install', name: 'Work Mac',
      sessions: [{ id: 'ses_mac', title: 'Build', updated_at: Date.now() }]
    })).status, 200);
    const replayed = await renewed.next(frame => frame.type === 'inbox' &&
      frame.messages.some(message => message.client_message_id === 'request-reconnect'));
    assert.equal(replayed.messages[0].client_message_id, 'request-reconnect');
    assert.equal((await renewed.request('ack-replay', 'ack', {
      ids: [replayed.messages[0].id]
    })).status, 200);
  } finally {
    const connected = [mac, phone, other, renewed].filter(Boolean)
      .filter(client => client.socket.readyState !== WebSocket.CLOSED);
    const closed = connected.map(client => once(client.socket, 'close'));
    for (const client of connected) client.socket.terminate();
    await Promise.all(closed);
    await new Promise(resolve => server.close(resolve));
    await new Promise(resolve => setTimeout(resolve, 20));
    db.close();
  }
});

test('WebSocket upgrade requires an authenticated account', async () => {
  const db = openSessionSyncStore(':memory:');
  const authorize = async () => null;
  const server = createServer(createSessionSyncService({ db, authorize }), authorize);
  server.listen(0, '127.0.0.1');
  await once(server, 'listening');
  try {
    const socket = new WebSocket(`ws://127.0.0.1:${server.address().port}/v1/session-sync/ws`);
    socket.on('error', () => {});
    const [, response] = await once(socket, 'unexpected-response');
    assert.equal(response.statusCode, 401);
    response.resume();
    socket.terminate();
  } finally {
    await new Promise(resolve => server.close(resolve));
    db.close();
  }
});

test('WebSocket refreshes its bearer without dropping the device connection', async () => {
  const db = openSessionSyncStore(':memory:');
  const original = 'a'.repeat(43);
  const renewed = 'b'.repeat(43);
  const anotherAccount = 'c'.repeat(43);
  const principal = { accountId: 'alice', slot: 'ios_phone',
    loginSessionId: 'phone-login' };
  const principals = new Map([[original, principal], [renewed, principal],
    [anotherAccount, { ...principal, accountId: 'bob' }]]);
  const authorize = async headers => principals.get(headers.authorization?.slice(7)) || null;
  const server = createServer(createSessionSyncService({ db, authorize }), authorize);
  server.listen(0, '127.0.0.1');
  await once(server, 'listening');
  const client = connected(`ws://127.0.0.1:${server.address().port}/v1/session-sync/ws`, original);
  try {
    await once(client.socket, 'open');
    assert.equal((await client.request('presence', 'presence', {
      device_id: 'phone-install', name: 'iPhone', sessions: []
    })).status, 200);
    principals.delete(original);
    assert.equal((await client.request('refresh', 'reauth', {
      access_token: renewed
    })).status, 200);
    assert.equal((await client.request('catalog', 'catalog')).status, 200);
    assert.equal(client.socket.readyState, WebSocket.OPEN);
    const closed = once(client.socket, 'close');
    client.socket.send(JSON.stringify({ id: 'wrong-account', op: 'reauth',
      body: { access_token: anotherAccount } }));
    assert.equal((await closed)[0], 1008);
  } finally {
    if (client.socket.readyState !== WebSocket.CLOSED) {
      const closed = once(client.socket, 'close');
      client.socket.terminate();
      await closed;
    }
    await new Promise(resolve => server.close(resolve));
    db.close();
  }
});
