'use strict';

const fs = require('node:fs');
const path = require('node:path');
const Database = require('better-sqlite3');
const { newAccountId } = require('./account-id');

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
      account_id TEXT UNIQUE,
      nickname TEXT,
      phone_e164 TEXT UNIQUE,
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
      revoked_at INTEGER,
      revoked_reason TEXT,
      device_slot TEXT CHECK(device_slot IN
        ('ios_phone', 'android_phone', 'tablet', 'mac', 'windows')),
      device_platform TEXT CHECK(device_platform IN
        ('ios', 'android', 'mac', 'windows'))
    );
    CREATE INDEX IF NOT EXISTS sessions_user ON sessions(user_id, revoked_at);
    CREATE INDEX IF NOT EXISTS sessions_previous_refresh
      ON sessions(previous_refresh_hash);
    CREATE TABLE IF NOT EXISTS desktop_login_requests (
      id TEXT PRIMARY KEY,
      platform TEXT NOT NULL CHECK(platform IN ('mac', 'windows')),
      code_challenge TEXT NOT NULL,
      display_code TEXT NOT NULL,
      ip_hash TEXT NOT NULL,
      status TEXT NOT NULL CHECK(status IN
        ('pending', 'scanned', 'approved', 'denied', 'consumed', 'canceled')),
      created_at INTEGER NOT NULL,
      expires_at INTEGER NOT NULL,
      approved_user_id TEXT REFERENCES users(id),
      approved_session_id TEXT REFERENCES sessions(id),
      approved_at INTEGER,
      consumed_at INTEGER,
      claimed_session_id TEXT REFERENCES sessions(id)
    );
    CREATE INDEX IF NOT EXISTS desktop_login_requests_ip_time
      ON desktop_login_requests(ip_hash, created_at);
    CREATE TABLE IF NOT EXISTS social_identities (
      provider TEXT NOT NULL CHECK(provider IN ('wechat', 'qq')),
      subject_hash TEXT NOT NULL,
      user_id TEXT NOT NULL REFERENCES users(id),
      created_at INTEGER NOT NULL,
      PRIMARY KEY(provider, subject_hash),
      UNIQUE(provider, user_id)
    );
    CREATE TABLE IF NOT EXISTS social_bind_intents (
      token_hash TEXT PRIMARY KEY,
      provider TEXT NOT NULL CHECK(provider IN ('wechat', 'qq')),
      subject_hash TEXT NOT NULL,
      device_slot TEXT NOT NULL CHECK(device_slot IN
        ('ios_phone', 'android_phone', 'tablet')),
      device_platform TEXT NOT NULL CHECK(device_platform IN ('ios', 'android')),
      created_at INTEGER NOT NULL,
      expires_at INTEGER NOT NULL,
      used_at INTEGER
    );
    CREATE INDEX IF NOT EXISTS social_bind_intents_expiry
      ON social_bind_intents(expires_at);
    CREATE TABLE IF NOT EXISTS social_verification_attempts (
      id INTEGER PRIMARY KEY AUTOINCREMENT,
      ip_hash TEXT NOT NULL,
      created_at INTEGER NOT NULL
    );
    CREATE INDEX IF NOT EXISTS social_verification_attempts_ip_time
      ON social_verification_attempts(ip_hash, created_at);
    CREATE TABLE IF NOT EXISTS password_failures (
      id INTEGER PRIMARY KEY AUTOINCREMENT,
      phone_hash TEXT NOT NULL,
      ip_hash TEXT NOT NULL,
      created_at INTEGER NOT NULL
    );
    CREATE INDEX IF NOT EXISTS password_failures_phone_time
      ON password_failures(phone_hash, created_at);
    CREATE TABLE IF NOT EXISTS friend_mirror (
      owner_user_id TEXT NOT NULL REFERENCES users(id),
      friend_user_id TEXT NOT NULL REFERENCES users(id),
      source TEXT NOT NULL DEFAULT 'tencent_im',
      observed_at INTEGER NOT NULL,
      PRIMARY KEY(owner_user_id, friend_user_id),
      CHECK(owner_user_id <> friend_user_id)
    );
    CREATE INDEX IF NOT EXISTS friend_mirror_friend
      ON friend_mirror(friend_user_id, owner_user_id);
    CREATE TABLE IF NOT EXISTS friend_mirror_sync (
      owner_user_id TEXT PRIMARY KEY REFERENCES users(id),
      synced_at INTEGER NOT NULL,
      im_friend_count INTEGER NOT NULL,
      mirrored_count INTEGER NOT NULL
    );
    CREATE TABLE IF NOT EXISTS friend_requests (
      id TEXT PRIMARY KEY,
      from_user_id TEXT NOT NULL REFERENCES users(id),
      to_user_id TEXT NOT NULL REFERENCES users(id),
      status TEXT NOT NULL CHECK(status IN ('pending','accepted','rejected')),
      created_at INTEGER NOT NULL,
      decided_at INTEGER,
      CHECK(from_user_id <> to_user_id)
    );
    CREATE INDEX IF NOT EXISTS friend_requests_to_status
      ON friend_requests(to_user_id,status,created_at);
    CREATE UNIQUE INDEX IF NOT EXISTS friend_requests_open
      ON friend_requests(from_user_id,to_user_id) WHERE status='pending';
    CREATE TABLE IF NOT EXISTS friendships (
      left_user_id TEXT NOT NULL REFERENCES users(id),
      right_user_id TEXT NOT NULL REFERENCES users(id),
      created_at INTEGER NOT NULL,
      PRIMARY KEY(left_user_id,right_user_id),
      CHECK(left_user_id < right_user_id)
    );
    CREATE TABLE IF NOT EXISTS friend_im_sync (
      left_user_id TEXT NOT NULL REFERENCES users(id),
      right_user_id TEXT NOT NULL REFERENCES users(id),
      desired TEXT NOT NULL CHECK(desired IN ('add','delete')),
      updated_at INTEGER NOT NULL,
      attempts INTEGER NOT NULL DEFAULT 0,
      last_error TEXT,
      revision INTEGER NOT NULL DEFAULT 1,
      PRIMARY KEY(left_user_id,right_user_id),
      CHECK(left_user_id < right_user_id)
    );
    CREATE TABLE IF NOT EXISTS profile_im_sync (
      user_id TEXT PRIMARY KEY REFERENCES users(id),
      desired_nickname TEXT NOT NULL,
      updated_at INTEGER NOT NULL,
      attempts INTEGER NOT NULL DEFAULT 0,
      last_error TEXT,
      revision INTEGER NOT NULL DEFAULT 1
    );
    CREATE TABLE IF NOT EXISTS admin_audit (
      id INTEGER PRIMARY KEY AUTOINCREMENT,
      action TEXT NOT NULL,
      target_user_id TEXT NOT NULL,
      created_at INTEGER NOT NULL
    );
    CREATE TABLE IF NOT EXISTS account_migrations (
      name TEXT PRIMARY KEY,
      applied_at INTEGER NOT NULL
    );
  `);
  const phoneColumn = db.prepare('PRAGMA table_info(users)').all()
    .find(column => column.name === 'phone_e164');
  if (phoneColumn?.notnull) {
    db.pragma('foreign_keys = OFF');
    try {
      db.transaction(() => {
        db.exec(`CREATE TABLE users_nullable_phone (
          id TEXT PRIMARY KEY,
          account_id TEXT UNIQUE,
          nickname TEXT,
          phone_e164 TEXT UNIQUE,
          im_user_id TEXT NOT NULL UNIQUE,
          password_hash TEXT,
          created_at INTEGER NOT NULL,
          updated_at INTEGER NOT NULL,
          disabled_at INTEGER
        );
        INSERT INTO users_nullable_phone SELECT id,NULL,NULL,phone_e164,im_user_id,
          password_hash,created_at,updated_at,disabled_at FROM users;
        DROP TABLE users;
        ALTER TABLE users_nullable_phone RENAME TO users;`);
      })();
    } finally {
      db.pragma('foreign_keys = ON');
    }
    if (db.pragma('foreign_key_check').length)
      throw new Error('User table migration failed foreign key validation');
  }
  const userColumns = new Set(db.pragma('table_info(users)').map(column => column.name));
  if (!userColumns.has('account_id')) db.exec('ALTER TABLE users ADD COLUMN account_id TEXT');
  if (!userColumns.has('nickname')) db.exec('ALTER TABLE users ADD COLUMN nickname TEXT');
  db.exec('CREATE UNIQUE INDEX IF NOT EXISTS users_account_id ON users(account_id)');
  db.transaction(() => {
    const legacy = db.prepare('SELECT id FROM users WHERE account_id IS NULL').all();
    for (const row of legacy)
      db.prepare('UPDATE users SET account_id=? WHERE id=?')
        .run(newAccountId(db), row.id);
  })();
  const syncColumns = new Set(db.pragma('table_info(friend_im_sync)')
    .map(column => column.name));
  if (!syncColumns.has('revision'))
    db.exec('ALTER TABLE friend_im_sync ADD COLUMN revision INTEGER NOT NULL DEFAULT 1');
  const sessionColumns = new Set(db.prepare('PRAGMA table_info(sessions)')
    .all().map(column => column.name));
  if (!sessionColumns.has('revoked_reason'))
    db.exec('ALTER TABLE sessions ADD COLUMN revoked_reason TEXT');
  if (!sessionColumns.has('device_slot'))
    db.exec(`ALTER TABLE sessions ADD COLUMN device_slot TEXT CHECK(device_slot IN
      ('ios_phone', 'android_phone', 'tablet', 'mac', 'windows'))`);
  if (!sessionColumns.has('device_platform'))
    db.exec(`ALTER TABLE sessions ADD COLUMN device_platform TEXT CHECK(device_platform IN
      ('ios', 'android', 'mac', 'windows'))`);
  const desktopTable = db.prepare(`SELECT sql FROM sqlite_master
    WHERE type='table' AND name='desktop_login_requests'`).get();
  if (!desktopTable.sql.includes("'canceled'")) {
    db.transaction(() => {
      db.exec(`CREATE TABLE desktop_login_requests_migrated (
        id TEXT PRIMARY KEY,
        platform TEXT NOT NULL CHECK(platform IN ('mac', 'windows')),
        code_challenge TEXT NOT NULL,
        display_code TEXT NOT NULL,
        ip_hash TEXT NOT NULL,
        status TEXT NOT NULL CHECK(status IN
          ('pending', 'scanned', 'approved', 'denied', 'consumed', 'canceled')),
        created_at INTEGER NOT NULL,
        expires_at INTEGER NOT NULL,
        approved_user_id TEXT REFERENCES users(id),
        approved_session_id TEXT REFERENCES sessions(id),
        approved_at INTEGER,
        consumed_at INTEGER,
        claimed_session_id TEXT REFERENCES sessions(id)
      );
      INSERT INTO desktop_login_requests_migrated
        (id,platform,code_challenge,display_code,ip_hash,status,created_at,
        expires_at,approved_user_id,approved_session_id,approved_at,consumed_at)
      SELECT id,platform,code_challenge,display_code,ip_hash,status,created_at,
        expires_at,approved_user_id,approved_session_id,approved_at,consumed_at
        FROM desktop_login_requests;
      DROP TABLE desktop_login_requests;
      ALTER TABLE desktop_login_requests_migrated RENAME TO desktop_login_requests;
      CREATE INDEX desktop_login_requests_ip_time
        ON desktop_login_requests(ip_hash, created_at);`);
    })();
  }
  const desktopColumns = new Set(db.prepare('PRAGMA table_info(desktop_login_requests)')
    .all().map(column => column.name));
  if (!desktopColumns.has('claimed_session_id'))
    db.exec(`ALTER TABLE desktop_login_requests ADD COLUMN claimed_session_id TEXT
      REFERENCES sessions(id)`);
  db.exec(`CREATE UNIQUE INDEX IF NOT EXISTS sessions_active_slot
    ON sessions(user_id, device_slot)
    WHERE revoked_at IS NULL AND device_slot IS NOT NULL`);
  if (filename !== ':memory:') fs.chmodSync(filename, 0o600);
  return db;
}

module.exports = { openStore };
