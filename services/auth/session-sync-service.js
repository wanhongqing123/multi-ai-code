'use strict';

const { createHash, randomUUID } = require('node:crypto');
const nodePath = require('node:path');

const DEVICE_SLOTS = new Set(['ios_phone', 'android_phone', 'tablet', 'mac', 'windows']);
const ONLINE_TIMEOUT_MS = 45_000;
const ENVELOPE_LIFETIME_MS = 10 * 60_000;
const EVENT_RETRY_MS = 30_000;
const MAX_PAYLOAD_BYTES = 64 * 1024;
const MAX_SESSIONS = 200;
const MAX_PENDING = 1000;
const RECEIPT_LIFETIME_MS = 30 * 24 * 60 * 60_000;
const MAX_ROOM_MESSAGE_BYTES = 48 * 1024;
const SESSION_LIFECYCLE_ID = 'session_lifecycle';

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
function validWorkspacePath(value) {
  return typeof value === 'string' && value.length > 0 &&
    Buffer.byteLength(value, 'utf8') <= 4096 &&
    !/[\u0000-\u001f\u007f-\u009f]/u.test(value) &&
    (nodePath.posix.isAbsolute(value) || nodePath.win32.isAbsolute(value));
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
      AND expires_at>? AND (delivered_at IS NULL OR
        (kind='event' AND delivered_at<=?))
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
    db.prepare(`UPDATE sync_envelopes SET delivered_at=NULL
      WHERE account_id=? AND to_slot=? AND to_login_session_id=?
        AND kind='event' AND acked_at IS NULL`).run(
      principal.accountId, principal.slot, principal.loginSessionId);
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
    const timestamp = now();
    const rows = pending.all(principal.accountId, principal.slot,
      principal.loginSessionId, timestamp, timestamp - EVENT_RETRY_MS);
    const markCommand = db.prepare(`UPDATE sync_envelopes SET delivered_at=?
      WHERE id=?`);
    for (const row of rows) markCommand.run(timestamp, row.id);
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

  const publishMessages = db.transaction((principal, sessionId, messages, permissions, questions) => {
    const upsert = db.prepare(`INSERT INTO sync_room_messages(account_id,owner_slot,session_id,
      message_id,payload,payload_hash,updated_at) VALUES(?,?,?,?,?,?,?)
      ON CONFLICT(account_id,owner_slot,session_id,message_id) DO UPDATE SET
      payload=excluded.payload,payload_hash=excluded.payload_hash,updated_at=excluded.updated_at`);
    const existing = db.prepare(`SELECT payload_hash FROM sync_room_messages WHERE account_id=?
      AND owner_slot=? AND session_id=? AND message_id=?`);
    const members = db.prepare(`SELECT slot,login_session_id FROM sync_devices WHERE account_id=?
      AND last_seen>?`).all(principal.accountId, now() - ONLINE_TIMEOUT_MS)
      .filter(member => member.slot !== principal.slot);
    const inboxCount = db.prepare(`SELECT COUNT(*) AS count FROM sync_envelopes WHERE account_id=?
      AND to_slot=? AND to_login_session_id=? AND acked_at IS NULL`);
    const deliver = db.prepare(`INSERT INTO sync_envelopes(account_id,to_slot,to_login_session_id,
      from_slot,from_login_session_id,client_message_id,session_id,kind,payload,created_at,
      expires_at) VALUES(?,?,?,?,?,?,?,?,?,?,?)`);
    const encodedPermissions = JSON.stringify(permissions);
    const encodedQuestions = JSON.stringify(questions);
    const previousState = db.prepare(`SELECT permissions,questions FROM sync_room_state
      WHERE account_id=? AND owner_slot=? AND session_id=?`).get(
      principal.accountId, principal.slot, sessionId);
    const stateChanged = previousState?.permissions !== encodedPermissions ||
      previousState?.questions !== encodedQuestions;
    if (stateChanged) db.prepare(`INSERT INTO sync_room_state(account_id,owner_slot,session_id,
      permissions,questions,updated_at) VALUES(?,?,?,?,?,?)
      ON CONFLICT(account_id,owner_slot,session_id) DO UPDATE SET
      permissions=excluded.permissions,questions=excluded.questions,
      updated_at=excluded.updated_at`).run(principal.accountId, principal.slot,
      sessionId, encodedPermissions, encodedQuestions, now());
    const changed = [];
    for (const message of messages) {
      const serialized = JSON.stringify(message);
      const hash = createHash('sha256').update(serialized).digest('hex');
      if (existing.get(principal.accountId, principal.slot, sessionId,
        message.id)?.payload_hash === hash) continue;
      upsert.run(principal.accountId, principal.slot, sessionId, message.id,
        serialized, hash, now());
      changed.push(message);
    }
    const updates = changed.map(message => [message]);
    if (!updates.length && stateChanged) updates.push([]);
    for (const update of updates) {
      const bytes = Buffer.from(JSON.stringify({ ok: true, messages: update,
        permissions, questions }));
      const transferID = randomUUID().replaceAll('-', '');
      const digest = createHash('sha256').update(bytes).digest('hex');
      const chunks = Math.ceil(bytes.length / (24 * 1024));
      for (const member of members) {
        if (inboxCount.get(principal.accountId, member.slot,
          member.login_session_id).count + chunks > MAX_PENDING) continue;
        for (let index = 0; index < chunks; index++) {
          const chunk = bytes.subarray(index * 24 * 1024, (index + 1) * 24 * 1024);
          deliver.run(principal.accountId, member.slot, member.login_session_id,
            principal.slot, principal.loginSessionId, randomUUID().replaceAll('-', ''),
            sessionId, 'event', JSON.stringify({ transfer_id: transferID,
              chunk_index: index, chunk_count: chunks, sha256: digest,
              data_base64: chunk.toString('base64') }), now(), now() + ENVELOPE_LIFETIME_MS);
        }
      }
    }
    return { changed, members, stateChanged };
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
            session.id === SESSION_LIFECYCLE_ID ||
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
      const devices = db.prepare(`SELECT slot AS device_slot,name AS device_name,
        device_id,login_session_id,last_seen FROM sync_devices WHERE account_id=?
        ORDER BY slot`).all(principal.accountId);
      const rows = db.prepare(`SELECT sessions.session_id AS id,sessions.title,
        sessions.updated_at,sessions.busy,sessions.slot AS device_slot,devices.name AS device_name,
        devices.device_id,devices.login_session_id,devices.last_seen
        FROM sync_sessions AS sessions JOIN sync_devices AS devices
        ON devices.account_id=sessions.account_id AND devices.slot=sessions.slot
        WHERE sessions.account_id=? ORDER BY sessions.updated_at DESC`)
        .all(principal.accountId);
      return reply(200, { sessions: rows.map(({ last_seen, ...row }) => ({ ...row,
        busy: row.busy === 1, online: last_seen > now() - ONLINE_TIMEOUT_MS })),
      devices: devices.map(({ last_seen, ...row }) => ({ ...row,
        online: last_seen > now() - ONLINE_TIMEOUT_MS })) });
    }
    if (method === 'GET' && url.pathname === '/v1/session-sync/history') {
      const slot = url.searchParams.get('owner_slot');
      const sessionId = url.searchParams.get('session_id');
      const before = url.searchParams.get('before') || '';
      const limit = Number(url.searchParams.get('limit') || 10);
      if (!DEVICE_SLOTS.has(slot) || !validId(sessionId) ||
          (before && !validId(before)) || !Number.isSafeInteger(limit) ||
          limit < 1 || limit > 20)
        return reply(400, { error: 'Invalid history page' });
      const owner = db.prepare(`SELECT 1 FROM sync_sessions WHERE account_id=? AND slot=?
        AND session_id=?`).get(principal.accountId, slot, sessionId);
      if (!owner) return reply(404, { error: 'Session is unavailable' });
      const rows = db.prepare(`SELECT payload FROM sync_room_messages WHERE account_id=?
        AND owner_slot=? AND session_id=? AND message_id<?
        ORDER BY message_id DESC LIMIT ?`).all(principal.accountId, slot,
        sessionId, before || '\uffff', limit);
      const state = db.prepare(`SELECT permissions,questions FROM sync_room_state WHERE
        account_id=? AND owner_slot=? AND session_id=?`).get(principal.accountId, slot, sessionId);
      return reply(200, { ok: true, messages: rows.map(row => JSON.parse(row.payload)),
        permissions: state ? JSON.parse(state.permissions) : [],
        questions: state ? JSON.parse(state.questions) : [] });
    }
    if (method === 'POST' && url.pathname === '/v1/session-sync/publish') {
      if (!isCurrent(principal)) return reply(409, { error: 'Device login was replaced' });
      if (!validId(body.session_id) || !Array.isArray(body.messages) ||
          body.messages.length > 20 ||
          (body.permissions !== undefined && !Array.isArray(body.permissions)) ||
          (body.questions !== undefined && !Array.isArray(body.questions)) ||
          Buffer.byteLength(JSON.stringify(body.permissions || [])) > 16 * 1024 ||
          Buffer.byteLength(JSON.stringify(body.questions || [])) > 16 * 1024 ||
          body.messages.some(message => !message || typeof message !== 'object' ||
            Array.isArray(message) || !validId(message.id) ||
            !['user', 'assistant', 'system'].includes(message.role) ||
            !Array.isArray(message.parts) ||
            Buffer.byteLength(JSON.stringify(message)) > MAX_ROOM_MESSAGE_BYTES) ||
          new Set(body.messages.map(message => message.id)).size !== body.messages.length)
        return reply(400, { error: 'Invalid room messages' });
      if (!currentSession.get(principal.accountId, principal.slot,
        principal.loginSessionId, body.session_id))
        return reply(404, { error: 'Session is unavailable' });
      const { changed, members, stateChanged } = publishMessages(principal,
        body.session_id, body.messages, body.permissions || [], body.questions || []);
      for (const member of members)
        if (changed.length || stateChanged)
          wake(principal.accountId, member.slot, member.login_session_id);
      return reply(200, { published: changed.length });
    }
    if (method === 'POST' && url.pathname === '/v1/session-sync/room-delete') {
      if (!isCurrent(principal)) return reply(409, { error: 'Device login was replaced' });
      if (!validId(body.session_id)) return reply(400, { error: 'Invalid Session ID' });
      const removed = db.transaction(() => {
        const messages = db.prepare(`DELETE FROM sync_room_messages WHERE account_id=?
          AND owner_slot=? AND session_id=?`).run(principal.accountId, principal.slot,
          body.session_id).changes;
        db.prepare(`DELETE FROM sync_room_state WHERE account_id=? AND owner_slot=?
          AND session_id=?`).run(principal.accountId, principal.slot, body.session_id);
        db.prepare(`DELETE FROM sync_envelopes WHERE account_id=? AND from_slot=?
          AND session_id=?`).run(principal.accountId, principal.slot, body.session_id);
        return messages;
      })();
      return reply(200, { removed });
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
          !['command', 'event'].includes(body.kind) ||
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
      if (body.session_id === SESSION_LIFECYCLE_ID) {
        if (body.kind === 'command') {
          if (body.payload.action === 'session_create') {
            const workspace = body.payload.workspace_path;
            if (workspace !== undefined && !validWorkspacePath(workspace))
              return reply(400, { error: 'Invalid workspace path' });
          } else if (body.payload.action === 'session_delete' ||
              body.payload.action === 'session_set_workspace') {
            if (!validId(body.payload.target_session_id))
              return reply(400, { error: 'Invalid target Session ID' });
            if (body.payload.action === 'session_set_workspace' &&
                !validWorkspacePath(body.payload.workspace_path))
              return reply(400, { error: 'Invalid workspace path' });
            if (!currentSession.get(principal.accountId, body.to_slot,
              body.to_login_session_id, body.payload.target_session_id))
              return reply(404, { error: 'Session is unavailable' });
          } else return reply(400, { error: 'Invalid Session lifecycle command' });
        } else {
          const result = body.payload.result;
          if (body.payload.type !== 'command_result' ||
              !validId(body.payload.command_id, 96) ||
              !result || typeof result !== 'object' || Array.isArray(result) ||
              typeof result.ok !== 'boolean' ||
              (result.ok && !validId(result.session_id)) ||
              (!result.ok && (!validTitle(result.error) || !result.error)) ||
              (result.session_id !== undefined && !validId(result.session_id)) ||
              (result.error !== undefined && !validTitle(result.error)))
            return reply(400, { error: 'Invalid Session lifecycle result' });
          const command = db.prepare(`SELECT 1 FROM sync_message_receipts
            WHERE account_id=? AND from_slot=? AND from_login_session_id=?
              AND client_message_id=? AND to_slot=? AND to_login_session_id=?
              AND session_id=? AND kind='command'`).get(principal.accountId,
            body.to_slot, body.to_login_session_id, body.payload.command_id,
            principal.slot, principal.loginSessionId, SESSION_LIFECYCLE_ID);
          if (!command) return reply(404, { error: 'Session command is unavailable' });
        }
      } else {
        if (body.kind === 'command' &&
            ['session_create', 'session_delete', 'session_set_workspace']
              .includes(body.payload.action))
          return reply(400, { error: 'Invalid Session lifecycle command' });
        const ownerSlot = body.kind === 'command'
          ? body.to_slot : principal.slot;
        const ownerLogin = body.kind === 'command'
          ? body.to_login_session_id : principal.loginSessionId;
        if (!currentSession.get(principal.accountId, ownerSlot, ownerLogin, body.session_id))
          return reply(404, { error: 'Session is unavailable' });
      }
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
      let acknowledged = 0;
      for (const id of body.ids) acknowledged += ack.run(now(), id, principal.accountId,
        principal.slot, principal.loginSessionId).changes;
      if (acknowledged) wake(principal.accountId, principal.slot, principal.loginSessionId);
      return reply(200, { acknowledged: true });
    }
    return reply(404, { error: 'Not found' });
  }

  return { handle, cleanup, subscribe, markDisconnected };
}

module.exports = { createSessionSyncService };
