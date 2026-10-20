// Operator-only completion of an account deletion after provider cleanup.
'use strict';

const fs = require('node:fs');
const { openStore } = require('./store');
const { completeAccountDeletion } = require('./account-deletion');

function main(args = process.argv.slice(2), env = process.env) {
  if (!env.MAICHAT_AUTH_DB_PATH || !env.MAICHAT_AUTH_HMAC_KEY)
    throw new Error('Auth database path and signing key are required');
  const db = openStore(env.MAICHAT_AUTH_DB_PATH);
  try {
    if (args.length === 1 && args[0] === '--list') {
      const pending = db.prepare(`SELECT id,account_id,im_user_id,phone_e164,
        requested_at,due_at FROM account_deletion_requests
        WHERE status='pending' ORDER BY requested_at`).all();
      process.stdout.write(`${JSON.stringify(pending, null, 2)}\n`);
      return;
    }
    if (args.length !== 3 || args[0] !== '--complete')
      throw new Error('Use --list or --complete <request-id> <evidence.json>');
    const file = args[2];
    const stat = fs.statSync(file);
    if (!stat.isFile() || stat.size > 16 * 1024 || (stat.mode & 0o077) !== 0)
      throw new Error('Evidence must be a private JSON file of at most 16 KB');
    const evidence = JSON.parse(fs.readFileSync(file, 'utf8'));
    const result = completeAccountDeletion(db, args[1], evidence,
      env.MAICHAT_AUTH_HMAC_KEY);
    process.stdout.write(`${JSON.stringify(result)}\n`);
  } finally {
    db.close();
  }
}

if (require.main === module) {
  try { main(); }
  catch (error) {
    process.stderr.write(`${error.message}\n`);
    process.exitCode = 1;
  }
}

module.exports = { main };
