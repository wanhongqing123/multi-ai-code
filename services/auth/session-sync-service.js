'use strict';

const { createHash } = require('node:crypto');

const DEVICE_SLOTS = new Set(['ios_phone', 'android_phone', 'tablet', 'mac', 'windows']);
const ONLINE_TIMEOUT_MS = 45_000;
const ENVELOPE_LIFETIME_MS = 10 * 60_000;
const MAX_PAYLOAD_BYTES = 64 * 1024;
const MAX_SESSIONS = 200;
const MAX_PENDING = 1000;
const RECEIPT_LIFETIME_MS = 30 * 24 * 60 * 60_000;

function reply(statusCode, body) { return { statusCode, body }; }
function validId(value, max = 128) {
  return typeof value === 'string' && value.length > 0 && value.length <= max &&
    /^[A-Za-z0-9_-]+$/.test(value);
}
function validTitle(value) {
  return typeof value === 'string' && [...value].length <= 120 &&
    Buffer.byteLength(value, 'utf8') <= 480 &&
    !/[\u0000-\u001f\u007f-\u009f]/u.test(value);
}

function createSessionSyncService({ db, authorize, now = Date.now }) {
  if (!db || typeof authorize !== 'function') throw new Error('Sync service needs a store and auth');
  const waiters = new Map();
  const mailboxes = new Map();
  const catalogs = new Map();
  const device = db.prepare('SELECT * FROM sync_devices WHERE account_id=? AND slot=?');
  const currentSession = db.prepare(`SELECT 1 FROM sync_sessions
    WHERE account_id=? AND slot=? AND login_session_id=? AND session_id=?`);
  const pending = db.prepare(`SELECT id,from_slot,from_login_session_id,
    client_message_id,session_id,kind,payload,created_at FROM sync_envelopes
    WHERE account_id=? AND to_slot=? AND to_login_session_id=? AND acked_at IS NULL
      AND expires_at>? AND (kind!='command' OR delivered_at IS NULL)
    ORDER BY id LIMIT 8`);

  function key(accountId, slot, loginSessionId) {
    return `${accountId}:${slot}:${loginSessionId}`;
  }
  function wake(accountId, slot, loginSessionId) {
    const target = key(accountId, slot, loginSessionId);
    const callbacks = waiters.get(target);
    waiters.delete(target);
    if (callbacks) for (const callback of callbacks) callback();
    for (const callback of mailboxes.get(target) || []) callback();
  }
  function publishCatalog(accountId) {
    for (const callback of catalogs.get(accountId) || []) callback();
  }
  function subscribe(principal, onMailbox, onCatalog) {
    const target = key(principal.accountId, principal.slot, principal.loginSessionId);
    const mailboxListeners = mailboxes.get(target) || new Set();
    const catalogListeners = catalogs.get(principal.accountId) || new Set();
    mailboxListeners.add(onMailbox);
    catalogListeners.add(onCatalog);
    mailboxes.set(target, mailboxListeners);
    catalogs.set(principal.accountId, catalogListeners);
    return () => {
      mailboxListeners.delete(onMailbox);
      catalogListeners.delete(onCatalog);
      if (!mailboxListeners.size) mailboxes.delete(target);
      if (!catalogListeners.size) catalogs.delete(principal.accountId);
    };
  }
  function markDisconnected(principal) {
    const changed = db.prepare(`UPDATE sync_devices SET last_seen=0
      WHERE account_id=? AND slot=? AND login_session_id=? AND last_seen!=0`).run(
      principal.accountId, principal.slot, principal.loginSessionId);
    if (changed.changes) publishCatalog(principal.accountId);
  }
  function isCurrent(principal) {
    const row = device.get(principal.accountId, principal.slot);
    return row && row.login_session_id === principal.loginSessionId;
  }
  const readPending = db.transaction(principal => {
    const rows = pending.all(principal.accountId, principal.slot,
      principal.loginSessionId, now());
    const markCommand = db.prepare(`UPDATE sync_envelopes SET delivered_at=?
      WHERE id=? AND delivered_at IS NULL`);
    for (const row of rows)
      if (row.kind === 'command') markCommand.run(now(), row.id);
    return rows.map(row => ({ ...row, payload: JSON.parse(row.payload) }));
  });
  function cleanup() {
    db.prepare('DELETE FROM sync_envelopes WHERE expires_at<=? OR acked_at<=?')
      .run(now(), now() - 60_000);
    db.prepare('DELETE FROM sync_message_receipts WHERE created_at<=?')
      .run(now() - RECEIPT_LIFETIME_MS);
  }
  const insertMessage = db.transaction((principal, body, payload, payloadHash) => {
    const inserted = db.prepare(`INSERT INTO sync_envelopes(account_id,to_slot,
      to_login_session_id,from_slot,from_login_session_id,client_message_id,session_id,
      kind,payload,created_at,expires_at) VALUES(?,?,?,?,?,?,?,?,?,?,?)`).run(
      principal.accountId, body.to_slot, body.to_login_session_id, principal.slot,
      principal.loginSessionId, body.client_message_id, body.session_id, body.kind,
      payload, now(), now() + ENVELOPE_LIFETIME_MS);
    const id = Number(inserted.lastInsertRowid);
    db.prepare(`INSERT INTO sync_message_receipts(account_id,from_slot,
      from_login_session_id,client_message_id,to_slot,to_login_session_id,
      session_id,kind,payload_hash,envelope_id,created_at)
      VALUES(?,?,?,?,?,?,?,?,?,?,?)`).run(principal.accountId, principal.slot,
      principal.loginSessionId, body.client_message_id, body.to_slot,
      body.to_login_session_id, body.session_id, body.kind, payloadHash, id, now());
    return id;
  });
  const updatePresence = db.transaction((principal, body) => {
    const previous = device.get(principal.accountId, principal.slot);
    if (previous && previous.login_session_id !== principal.loginSessionId) {
      db.prepare(`DELETE FROM sync_envelopes WHERE account_id=? AND to_slot=?
        AND to_login_session_id=?`).run(principal.accountId, principal.slot,
        previous.login_session_id);
    }
    db.prepare(`INSERT INTO sync_devices(account_id,slot,login_session_id,device_id,name,last_seen)
      VALUES(?,?,?,?,?,?) ON CONFLICT(account_id,slot) DO UPDATE SET
      login_session_id=excluded.login_session_id,device_id=excluded.device_id,
      name=excluded.name,last_seen=excluded.last_seen`).run(principal.accountId,
      principal.slot, principal.loginSessionId, body.device_id, body.name, now());
    db.prepare('DELETE FROM sync_sessions WHERE account_id=? AND slot=?')
      .run(principal.accountId, principal.slot);
    const insert = db.prepare(`INSERT INTO sync_sessions(account_id,slot,login_session_id,
      session_id,title,updated_at,busy) VALUES(?,?,?,?,?,?,?)`);
    for (const session of body.sessions) insert.run(principal.accountId, principal.slot,
      principal.loginSessionId, session.id, session.title, session.updated_at,
      session.busy === true ? 1 : 0);
    return previous?.login_session_id;
  });

  async function poll(principal, waitMs) {
    if (!isCurrent(principal)) return reply(409, { error: 'Device login was replaced' });
    let messages = readPending(principal);
    if (messages.length || !waitMs) return reply(200, { messages });
    await new Promise(resolve => {
      const target = key(principal.accountId, principal.slot, principal.loginSessionId);
      const callbacks = waiters.get(target) || new Set();
      waiters.set(target, callbacks);
      const timer = setTimeout(() => {
        callbacks.delete(done);
        if (!callbacks.size) waiters.delete(target);
        resolve();
      }, waitMs);
      function done() { clearTimeout(timer); resolve(); }
      callbacks.add(done);
    });
    if (!isCurrent(principal)) return reply(409, { error: 'Device login was replaced' });
    messages = readPending(principal);
    return reply(200, { messages });
  }

  async function handle({ method, path, body = {}, headers = {} }) {
    const principal = await authorize(headers);
    if (!principal) return reply(401, { error: 'Authentication required' });
    if (!validId(principal.accountId, 64) || !DEVICE_SLOTS.has(principal.slot) ||
        !validId(principal.loginSessionId))
      return reply(403, { error: 'An updated device login is required' });
    const url = new URL(path, 'http://localhost');
    cleanup();
    if (method === 'POST' && url.pathname === '/v1/session-sync/presence') {
      if (!validId(body.device_id, 64) || !validTitle(body.name) ||
          !body.name || !Array.isArray(body.sessions) ||
          body.sessions.length > MAX_SESSIONS ||
          body.sessions.some(session => !validId(session?.id) ||
            !validTitle(session?.title) || !Number.isSafeInteger(session?.updated_at) ||
            session.updated_at < 0 ||
            (session.busy !== undefined && typeof session.busy !== 'boolean')))
        return reply(400, { error: 'Invalid device presence' });
      if (new Set(body.sessions.map(session => session.id)).size !== body.sessions.length)
        return reply(400, { error: 'Duplicate Session ID' });
      const previousLogin = updatePresence(principal, body);
      if (previousLogin && previousLogin !== principal.loginSessionId)
        wake(principal.accountId, principal.slot, previousLogin);
      publishCatalog(principal.accountId);
      return reply(200, { online: true, login_session_id: principal.loginSessionId });
    }
    if (method === 'GET' && url.pathname === '/v1/session-sync/sessions') {
      const rows = db.prepare(`SELECT sessions.session_id AS id,sessions.title,
        sessions.updated_at,sessions.busy,sessions.slot AS device_slot,devices.name AS device_name,
        devices.device_id,devices.login_session_id,devices.last_seen
        FROM sync_sessions AS sessions JOIN sync_devices AS devices
        ON devices.account_id=sessions.account_id AND devices.slot=sessions.slot
        WHERE sessions.account_id=? ORDER BY sessions.updated_at DESC`)
        .all(principal.accountId);
      return reply(200, { sessions: rows.map(({ last_seen, ...row }) => ({ ...row,
        busy: row.busy === 1, online: last_seen > now() - ONLINE_TIMEOUT_MS })) });
    }
    if (method === 'GET' && url.pathname === '/v1/session-sync/poll') {
      const waitMs = Number(url.searchParams.get('wait_ms') || 0);
      if (!Number.isSafeInteger(waitMs) || waitMs < 0 || waitMs > 20_000)
        return reply(400, { error: 'Invalid wait time' });
      return poll(principal, waitMs);
    }
    if (method === 'POST' && url.pathname === '/v1/session-sync/send') {
      if (!isCurrent(principal)) return reply(409, { error: 'Device login was replaced' });
      if (!DEVICE_SLOTS.has(body.to_slot) || !validId(body.to_login_session_id) ||
          !validId(body.client_message_id, 96) || !validId(body.session_id) ||
          !['command', 'history_request', 'event', 'history_page'].includes(body.kind) ||
          !body.payload || typeof body.payload !== 'object' || Array.isArray(body.payload))
        return reply(400, { error: 'Invalid device message' });
      const payload = JSON.stringify(body.payload);
      if (Buffer.byteLength(payload) > MAX_PAYLOAD_BYTES)
        return reply(413, { error: 'Device message is too large' });
      const payloadHash = createHash('sha256').update(payload).digest('hex');
      const previous = db.prepare(`SELECT envelope_id,payload_hash,kind,session_id,to_slot,
        to_login_session_id FROM sync_message_receipts WHERE account_id=? AND from_slot=?
        AND from_login_session_id=? AND client_message_id=?`).get(principal.accountId,
        principal.slot, principal.loginSessionId, body.client_message_id);
      if (previous) {
        if (previous.payload_hash !== payloadHash || previous.kind !== body.kind ||
            previous.session_id !== body.session_id || previous.to_slot !== body.to_slot ||
            previous.to_login_session_id !== body.to_login_session_id)
          return reply(409, { error: 'Message ID was reused for different content' });
        return reply(200, { id: previous.envelope_id, duplicate: true });
      }
      const recipient = device.get(principal.accountId, body.to_slot);
      if (!recipient || recipient.login_session_id !== body.to_login_session_id ||
          recipient.last_seen <= now() - ONLINE_TIMEOUT_MS)
        return reply(409, { error: 'Target device is offline or was replaced' });
      const ownerSlot = ['command', 'history_request'].includes(body.kind)
        ? body.to_slot : principal.slot;
      const ownerLogin = ['command', 'history_request'].includes(body.kind)
        ? body.to_login_session_id : principal.loginSessionId;
      if (!currentSession.get(principal.accountId, ownerSlot, ownerLogin, body.session_id))
        return reply(404, { error: 'Session is unavailable' });
      const count = db.prepare(`SELECT COUNT(*) AS count FROM sync_envelopes
        WHERE account_id=? AND to_slot=? AND to_login_session_id=? AND acked_at IS NULL`)
        .get(principal.accountId, body.to_slot, body.to_login_session_id).count;
      if (count >= MAX_PENDING) return reply(429, { error: 'Device inbox is full' });
      const id = insertMessage(principal, body, payload, payloadHash);
      wake(principal.accountId, body.to_slot, body.to_login_session_id);
      return reply(200, { id, duplicate: false });
    }
    if (method === 'POST' && url.pathname === '/v1/session-sync/ack') {
      if (!isCurrent(principal)) return reply(409, { error: 'Device login was replaced' });
      if (!Array.isArray(body.ids) || body.ids.length > 32 ||
          body.ids.some(id => !Number.isSafeInteger(id) || id <= 0))
        return reply(400, { error: 'Invalid acknowledgement' });
      const ack = db.prepare(`UPDATE sync_envelopes SET acked_at=? WHERE id=?
        AND account_id=? AND to_slot=? AND to_login_session_id=?`);
      for (const id of body.ids) ack.run(now(), id, principal.accountId,
        principal.slot, principal.loginSessionId);
      return reply(200, { acknowledged: true });
    }
    return reply(404, { error: 'Not found' });
  }

  return { handle, cleanup, subscribe, markDisconnected };
}

module.exports = { createSessionSyncService };
