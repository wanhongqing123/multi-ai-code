// Operator-only removal of account-scoped Session sync rows.
'use strict';

const Database = require('better-sqlite3');

const TABLES = ['sync_envelopes', 'sync_message_receipts',
  'sync_sessions', 'sync_devices'];

function main(args = process.argv.slice(2), env = process.env) {
  const execute = args[0] === '--execute';
  const requestId = execute ? args[1] : args[0];
  if (args.length !== (execute ? 2 : 1) ||
      !/^[0-9a-f-]{36}$/.test(requestId || '') ||
      !env.MAICHAT_AUTH_DB_PATH || !env.MAICHAT_SESSION_SYNC_DB_PATH)
    throw new Error('Use [--execute] <deletion-request-id> with both database paths');
  const auth = new Database(env.MAICHAT_AUTH_DB_PATH,
    { readonly: true, fileMustExist: true });
  let request;
  try {
    request = auth.prepare(`SELECT account_id FROM account_deletion_requests
      WHERE id=? AND status='pending'`).get(requestId);
  } finally { auth.close(); }
  if (!request?.account_id) throw new Error('Pending deletion request not found');
  const sync = new Database(env.MAICHAT_SESSION_SYNC_DB_PATH,
    { timeout: 5000, fileMustExist: true });
  try {
    const counts = Object.fromEntries(TABLES.map(table => [table,
      sync.prepare(`SELECT COUNT(*) AS count FROM ${table} WHERE account_id=?`)
        .get(request.account_id).count]));
    if (execute) sync.transaction(() => {
      for (const table of TABLES)
        sync.prepare(`DELETE FROM ${table} WHERE account_id=?`).run(request.account_id);
    })();
    process.stdout.write(`${JSON.stringify({ request_id: requestId,
      account_id: request.account_id, execute, counts })}\n`);
  } finally { sync.close(); }
}

if (require.main === module) {
  try { main(); }
  catch (error) {
    process.stderr.write(`${error.message}\n`);
    process.exitCode = 1;
  }
}

module.exports = { main };
