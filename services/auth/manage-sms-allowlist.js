'use strict';

const fs = require('node:fs');
const { openStore } = require('./store');
const { normalizePhone } = require('./service');

// 仅供服务器管理员本机使用，不提供公网管理接口。
// 从 stdin 读取号码，避免它出现在 shell 命令参数和进程列表中。
function manageSmsAllowlist(db, action, phoneInput, now = Date.now()) {
  const phone = normalizePhone(phoneInput);
  if (!phone || !['add', 'remove', 'check'].includes(action))
    throw new Error('Invalid allowlist action or phone');
  if (action === 'add') {
    const result = db.prepare(`INSERT OR IGNORE INTO sms_allowed_phones(phone_e164,created_at)
      VALUES(?,?)`).run(phone, now);
    return { allowed: true, changed: result.changes === 1 };
  }
  if (action === 'remove') {
    const result = db.prepare('DELETE FROM sms_allowed_phones WHERE phone_e164=?').run(phone);
    return { allowed: false, changed: result.changes === 1 };
  }
  return {
    allowed: !!db.prepare('SELECT 1 FROM sms_allowed_phones WHERE phone_e164=?').get(phone),
    changed: false
  };
}

if (require.main === module) {
  const databasePath = process.env.MAICHAT_AUTH_DB_PATH;
  if (!databasePath) throw new Error('Auth database path is missing');
  const input = fs.readFileSync(0, 'utf8');
  if (Buffer.byteLength(input, 'utf8') > 1024) throw new Error('Request is too large');
  const request = JSON.parse(input);
  const db = openStore(databasePath);
  try {
    const result = manageSmsAllowlist(db, request.action, request.phone);
    process.stdout.write(JSON.stringify(result) + '\n');
  } finally {
    db.close();
  }
}

module.exports = { manageSmsAllowlist };
