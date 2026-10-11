'use strict';

const fs = require('node:fs');
const { openStore } = require('./store');
const { normalizePhone } = require('./service');

// 仅供服务器管理员在核实旧账号归属后使用；绝不暴露成公网 API。
// 从 stdin 读入 JSON，避免手机号在 shell 参数和进程列表中出现。
function bindLegacy(db, phoneInput, imUserId, now = Date.now()) {
  const phone = normalizePhone(phoneInput);
  if (!phone || typeof imUserId !== 'string' || !/^[A-Za-z0-9_-]{1,32}$/.test(imUserId))
    throw new Error('Invalid phone or legacy IM UserID');
  return db.transaction(() => {
    if (db.prepare('SELECT id FROM users WHERE phone_e164=?').get(phone))
      throw new Error('Phone already owns a MaiChat account');
    if (db.prepare('SELECT id FROM users WHERE im_user_id=?').get(imUserId))
      throw new Error('IM UserID already belongs to another MaiChat account');
    db.prepare(`INSERT INTO legacy_im_bindings(phone_e164,im_user_id,created_at)
      VALUES(?,?,?)`).run(phone, imUserId, now);
  })();
}

if (require.main === module) {
  const databasePath = process.env.MAICHAT_AUTH_DB_PATH;
  if (!databasePath) throw new Error('Auth database path is missing');
  const input = fs.readFileSync(0, 'utf8');
  if (Buffer.byteLength(input, 'utf8') > 1024) throw new Error('Binding request is too large');
  const request = JSON.parse(input);
  const db = openStore(databasePath);
  try {
    bindLegacy(db, request.phone, request.im_user_id);
    process.stdout.write('Legacy IM binding prepared\n');
  } finally {
    db.close();
  }
}

module.exports = { bindLegacy };
