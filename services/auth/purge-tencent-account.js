// Operator command; never runs from a user-facing HTTP request.
'use strict';

const { openStore } = require('./store');
const { createTencentAccountDeleter } = require('./tim-account-delete');

async function main(args = process.argv.slice(2), env = process.env) {
  const execute = args[0] === '--execute';
  const requestId = execute ? args[1] : args[0];
  if (args.length !== (execute ? 2 : 1) ||
      !/^[0-9a-f-]{36}$/.test(requestId || '') || !env.MAICHAT_AUTH_DB_PATH)
    throw new Error('Use [--execute] <deletion-request-id>');
  const db = openStore(env.MAICHAT_AUTH_DB_PATH);
  let request;
  try {
    request = db.prepare(`SELECT im_user_id FROM account_deletion_requests
      WHERE id=? AND status='pending'`).get(requestId);
  } finally { db.close(); }
  if (!request?.im_user_id) throw new Error('Pending deletion request not found');
  if (!execute) {
    process.stdout.write(`${JSON.stringify({ request_id: requestId,
      im_user_id: request.im_user_id, execute: false })}\n`);
    return;
  }
  const remove = createTencentAccountDeleter({
    sdkAppId: Number(env.MAICHAT_IM_SDK_APP_ID),
    imSecret: env.MAICHAT_IM_SECRET_KEY,
    adminUser: env.MAICHAT_IM_ADMIN_USER || 'administrator',
    origin: env.MAICHAT_IM_REST_ORIGIN || 'https://console.tim.qq.com'
  });
  const result = await remove(request.im_user_id);
  process.stdout.write(`${JSON.stringify({ request_id: requestId, ...result })}\n`);
}

if (require.main === module) {
  main().catch(error => {
    process.stderr.write(`${error.message}\n`);
    process.exitCode = 1;
  });
}

module.exports = { main };
