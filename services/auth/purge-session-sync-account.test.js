'use strict';

const assert = require('node:assert/strict');
const { mkdtempSync, rmSync } = require('node:fs');
const { tmpdir } = require('node:os');
const { join } = require('node:path');
const test = require('node:test');
const Database = require('better-sqlite3');
const { openSessionSyncStore } = require('./session-sync-store');
const { main } = require('./purge-session-sync-account');

test('account deletion purges persisted Session room messages and state', () => {
  const directory = mkdtempSync(join(tmpdir(), 'maichat-room-purge-'));
  const authPath = join(directory, 'auth.db');
  const syncPath = join(directory, 'sync.db');
  const requestId = 'aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa';
  const env = { MAICHAT_AUTH_DB_PATH: authPath,
    MAICHAT_SESSION_SYNC_DB_PATH: syncPath };
  const auth = new Database(authPath);
  const sync = openSessionSyncStore(syncPath);
  try {
    auth.exec(`CREATE TABLE account_deletion_requests (
      id TEXT PRIMARY KEY, account_id TEXT, status TEXT)`);
    auth.prepare(`INSERT INTO account_deletion_requests(id,account_id,status)
      VALUES(?,?,'pending')`).run(requestId, 'alice');
    sync.prepare(`INSERT INTO sync_room_messages(account_id,owner_slot,session_id,
      message_id,payload,payload_hash,updated_at) VALUES(?,?,?,?,?,?,?)`)
      .run('alice', 'mac', 'ses_1', 'msg_1', '{}', 'hash', 1);
    sync.prepare(`INSERT INTO sync_room_state(account_id,owner_slot,session_id,
      permissions,questions,updated_at) VALUES(?,?,?,?,?,?)`)
      .run('alice', 'mac', 'ses_1', '[]', '[]', 1);
    main([requestId], env);
    assert.equal(sync.prepare(`SELECT COUNT(*) AS count FROM sync_room_messages`).get().count, 1);
    main(['--execute', requestId], env);
    assert.equal(sync.prepare(`SELECT COUNT(*) AS count FROM sync_room_messages`).get().count, 0);
    assert.equal(sync.prepare(`SELECT COUNT(*) AS count FROM sync_room_state`).get().count, 0);
  } finally {
    sync.close();
    auth.close();
    rmSync(directory, { recursive: true, force: true });
  }
});
