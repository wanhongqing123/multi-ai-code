'use strict';

const fs = require('node:fs');
const path = require('node:path');
const Database = require('better-sqlite3');

function openStore(filename) {
  if (filename !== ':memory:') {
    fs.mkdirSync(path.dirname(filename), { recursive: true, mode: 0o700 });
  }
  const db = new Database(filename, { timeout: 5000 });
  db.pragma('journal_mode = WAL');
  db.pragma('foreign_keys = ON');
  db.pragma('busy_timeout = 5000');
  db.exec(`
    CREATE TABLE IF NOT EXISTS users (
      id TEXT PRIMARY KEY,
      phone_e164 TEXT NOT NULL UNIQUE,
      im_user_id TEXT NOT NULL UNIQUE,
      password_hash TEXT,
      created_at INTEGER NOT NULL,
      updated_at INTEGER NOT NULL,
      disabled_at INTEGER
    );
    CREATE TABLE IF NOT EXISTS legacy_im_bindings (
      phone_e164 TEXT PRIMARY KEY,
      im_user_id TEXT NOT NULL UNIQUE,
      created_at INTEGER NOT NULL,
      used_at INTEGER
    );
    CREATE TABLE IF NOT EXISTS sms_challenges (
      id TEXT PRIMARY KEY,
      phone_hash TEXT NOT NULL,
      ip_hash TEXT NOT NULL,
      purpose TEXT NOT NULL,
      code_hash TEXT NOT NULL,
      status TEXT NOT NULL,
      attempts INTEGER NOT NULL DEFAULT 0,
      created_at INTEGER NOT NULL,
      expires_at INTEGER NOT NULL
    );
    CREATE INDEX IF NOT EXISTS sms_challenges_phone_time
      ON sms_challenges(phone_hash, created_at);
    CREATE INDEX IF NOT EXISTS sms_challenges_ip_time
      ON sms_challenges(ip_hash, created_at);
    CREATE TABLE IF NOT EXISTS sessions (
      id TEXT PRIMARY KEY,
      user_id TEXT NOT NULL REFERENCES users(id),
      access_hash TEXT NOT NULL UNIQUE,
      access_expires_at INTEGER NOT NULL,
      refresh_hash TEXT NOT NULL UNIQUE,
      previous_refresh_hash TEXT,
      refresh_expires_at INTEGER NOT NULL,
      auth_time INTEGER NOT NULL,
      created_at INTEGER NOT NULL,
      revoked_at INTEGER
    );
    CREATE INDEX IF NOT EXISTS sessions_user ON sessions(user_id, revoked_at);
    CREATE INDEX IF NOT EXISTS sessions_previous_refresh
      ON sessions(previous_refresh_hash);
    CREATE TABLE IF NOT EXISTS password_failures (
      id INTEGER PRIMARY KEY AUTOINCREMENT,
      phone_hash TEXT NOT NULL,
      ip_hash TEXT NOT NULL,
      created_at INTEGER NOT NULL
    );
    CREATE INDEX IF NOT EXISTS password_failures_phone_time
      ON password_failures(phone_hash, created_at);
  `);
  if (filename !== ':memory:') fs.chmodSync(filename, 0o600);
  return db;
}

module.exports = { openStore };
