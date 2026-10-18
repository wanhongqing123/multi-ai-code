// One-time import of reciprocal Tencent relationships before MaiChat becomes authoritative.
'use strict';

const { openStore } = require('./store');

const MIGRATION = 'reciprocal_friend_mirror_20261018';

function importFriendMirror(db, { dryRun = false, now = Date.now } = {}) {
  const edges = db.prepare(`SELECT a.owner_user_id AS left_user_id,
    a.friend_user_id AS right_user_id FROM friend_mirror a
    JOIN friend_mirror b ON b.owner_user_id=a.friend_user_id
      AND b.friend_user_id=a.owner_user_id
    WHERE a.owner_user_id<a.friend_user_id ORDER BY a.owner_user_id`).all();
  const directedCount = db.prepare('SELECT COUNT(*) AS count FROM friend_mirror').get().count;
  const alreadyApplied = !!db.prepare('SELECT 1 FROM account_migrations WHERE name=?')
    .get(MIGRATION);
  if (dryRun || alreadyApplied)
    return { directed_count: directedCount, mutual_pairs: edges.length,
      imported: 0, already_applied: alreadyApplied };
  const imported = db.transaction(() => {
    let count = 0;
    for (const edge of edges) {
      count += db.prepare(`INSERT OR IGNORE INTO friendships
        (left_user_id,right_user_id,created_at) VALUES(?,?,?)`)
        .run(edge.left_user_id, edge.right_user_id, now()).changes;
    }
    db.prepare('INSERT INTO account_migrations(name,applied_at) VALUES(?,?)')
      .run(MIGRATION, now());
    return count;
  })();
  return { directed_count: directedCount, mutual_pairs: edges.length,
    imported, already_applied: false };
}

if (require.main === module) {
  const mode = process.argv[2];
  if (!['--dry-run', '--run'].includes(mode) || !process.env.MAICHAT_AUTH_DB_PATH)
    throw new Error('Usage: MAICHAT_AUTH_DB_PATH=... node import-friend-mirror.js --dry-run|--run');
  const db = openStore(process.env.MAICHAT_AUTH_DB_PATH);
  try { process.stdout.write(`${JSON.stringify(importFriendMirror(db,
    { dryRun: mode === '--dry-run' }))}\n`); }
  finally { db.close(); }
}

module.exports = { importFriendMirror };
