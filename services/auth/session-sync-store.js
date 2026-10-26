'use strict';

const fs = require('node:fs');
const path = require('node:path');
const Database = require('better-sqlite3');

function openSessionSyncStore(filename) {
  if (filename !== ':memory:')
    fs.mkdirSync(path.dirname(filename), { recursive: true, mode: 0o700 });
  const db = new Database(filename, { timeout: 5000 });
  db.pragma('journal_mode = WAL');
  db.pragma('foreign_keys = ON');
  db.pragma('busy_timeout = 5000');
  db.exec(`
    CREATE TABLE IF NOT EXISTS sync_devices (
      account_id TEXT NOT NULL,
      slot TEXT NOT NULL,
      login_session_id TEXT NOT NULL,
      device_id TEXT NOT NULL,
      name TEXT NOT NULL,
      last_seen INTEGER NOT NULL,
      PRIMARY KEY(account_id, slot)
    );
    CREATE TABLE IF NOT EXISTS sync_sessions (
      account_id TEXT NOT NULL,
      slot TEXT NOT NULL,
      login_session_id TEXT NOT NULL,
      session_id TEXT NOT NULL,
      title TEXT NOT NULL,
      updated_at INTEGER NOT NULL,
      busy INTEGER NOT NULL DEFAULT 0,
      PRIMARY KEY(account_id, slot, session_id)
    );
    CREATE INDEX IF NOT EXISTS sync_sessions_account_updated
      ON sync_sessions(account_id, updated_at DESC);
    CREATE TABLE IF NOT EXISTS sync_envelopes (
      id INTEGER PRIMARY KEY AUTOINCREMENT,
      account_id TEXT NOT NULL,
      to_slot TEXT NOT NULL,
      to_login_session_id TEXT NOT NULL,
      from_slot TEXT NOT NULL,
      from_login_session_id TEXT NOT NULL,
      client_message_id TEXT NOT NULL,
      session_id TEXT NOT NULL,
      kind TEXT NOT NULL,
      payload TEXT NOT NULL,
      created_at INTEGER NOT NULL,
      expires_at INTEGER NOT NULL,
      delivered_at INTEGER,
      acked_at INTEGER,
      UNIQUE(account_id, from_slot, from_login_session_id, client_message_id)
    );
    CREATE INDEX IF NOT EXISTS sync_envelopes_pending
      ON sync_envelopes(account_id, to_slot, to_login_session_id, acked_at, id);
    CREATE TABLE IF NOT EXISTS sync_message_receipts (
      account_id TEXT NOT NULL,
      from_slot TEXT NOT NULL,
      from_login_session_id TEXT NOT NULL,
      client_message_id TEXT NOT NULL,
      to_slot TEXT NOT NULL,
      to_login_session_id TEXT NOT NULL,
      session_id TEXT NOT NULL,
      kind TEXT NOT NULL,
      payload_hash TEXT NOT NULL,
      envelope_id INTEGER NOT NULL,
      created_at INTEGER NOT NULL,
      PRIMARY KEY(account_id, from_slot, from_login_session_id, client_message_id)
    );
    CREATE INDEX IF NOT EXISTS sync_message_receipts_age
      ON sync_message_receipts(created_at);
    CREATE TABLE IF NOT EXISTS sync_room_messages (
      account_id TEXT NOT NULL,
      owner_slot TEXT NOT NULL,
      session_id TEXT NOT NULL,
      message_id TEXT NOT NULL,
      payload TEXT NOT NULL,
      payload_hash TEXT NOT NULL,
      updated_at INTEGER NOT NULL,
      PRIMARY KEY(account_id, owner_slot, session_id, message_id)
    );
    CREATE INDEX IF NOT EXISTS sync_room_messages_page
      ON sync_room_messages(account_id, owner_slot, session_id, message_id DESC);
    CREATE TABLE IF NOT EXISTS sync_room_state (
      account_id TEXT NOT NULL,
      owner_slot TEXT NOT NULL,
      session_id TEXT NOT NULL,
      permissions TEXT NOT NULL,
      questions TEXT NOT NULL,
      updated_at INTEGER NOT NULL,
      PRIMARY KEY(account_id, owner_slot, session_id)
    );
  `);
  const envelopeColumns = new Set(db.pragma('table_info(sync_envelopes)').map(row => row.name));
  if (!envelopeColumns.has('delivered_at'))
    db.exec('ALTER TABLE sync_envelopes ADD COLUMN delivered_at INTEGER');
  const sessionColumns = new Set(db.pragma('table_info(sync_sessions)').map(row => row.name));
  if (!sessionColumns.has('busy'))
    db.exec('ALTER TABLE sync_sessions ADD COLUMN busy INTEGER NOT NULL DEFAULT 0');
  // 进程重启后，旧心跳不能证明设备仍连着新进程；等待它重新上报。
  db.prepare('UPDATE sync_devices SET last_seen=0').run();
  // 未确认的事件可在新连接上立即重发；命令仍保持首次投递语义。
  db.prepare(`UPDATE sync_envelopes SET delivered_at=NULL
    WHERE kind='event' AND acked_at IS NULL`).run();
  return db;
}

module.exports = { openSessionSyncStore };
