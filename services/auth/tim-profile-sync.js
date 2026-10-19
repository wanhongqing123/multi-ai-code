// Project MaiChat nicknames into Tencent IM without making IM the source of truth.
'use strict';

const { randomInt } = require('node:crypto');
const { createUserSig } = require('./usersig');

function createTencentProfileWriter({ sdkAppId, imSecret, adminUser = 'administrator',
  origin = 'https://console.tim.qq.com', fetchImpl = fetch }) {
  if (!Number.isSafeInteger(sdkAppId) || sdkAppId <= 0 ||
      typeof imSecret !== 'string' || imSecret.length < 32 ||
      !/^[A-Za-z0-9_-]{1,32}$/.test(adminUser) || !origin.startsWith('https://'))
    throw new Error('IM profile writer configuration is invalid');
  return async ({ imUserId, nickname }) => {
    if (typeof imUserId !== 'string' || !/^[A-Za-z0-9_-]{1,32}$/.test(imUserId) ||
        typeof nickname !== 'string' || [...nickname].length > 32 ||
        Buffer.byteLength(nickname, 'utf8') > 120)
      throw new Error('Invalid IM profile operation');
    const url = new URL('/v4/profile/portrait_set', origin);
    url.search = new URLSearchParams({ sdkappid: String(sdkAppId), identifier: adminUser,
      usersig: createUserSig(sdkAppId, adminUser, imSecret, Math.floor(Date.now() / 1000)),
      random: String(randomInt(1, 0xFFFFFFFF)), contenttype: 'json' }).toString();
    const response = await fetchImpl(url, { method: 'POST', redirect: 'error',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ From_Account: imUserId,
        ProfileItem: [{ Tag: 'Tag_Profile_IM_Nick', Value: nickname }] }),
      signal: AbortSignal.timeout(10_000) });
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    const text = await response.text();
    if (text.length > 64 * 1024) throw new Error('IM response is too large');
    let result;
    try { result = JSON.parse(text); } catch { throw new Error('Invalid IM response'); }
    if (result.ActionStatus !== 'OK' || result.ErrorCode !== 0)
      throw new Error(`IM error ${result.ErrorCode ?? 'unknown'}`);
  };
}

async function syncPendingProfileActions(db, writer, limit = 50) {
  db.prepare(`DELETE FROM profile_im_sync WHERE user_id IN
    (SELECT id FROM users WHERE disabled_at IS NOT NULL)`).run();
  const rows = db.prepare(`SELECT profile_im_sync.*,users.im_user_id FROM profile_im_sync
    JOIN users ON users.id=profile_im_sync.user_id
    ORDER BY updated_at LIMIT ?`).all(limit);
  let completed = 0;
  for (const row of rows) {
    try {
      await writer({ imUserId: row.im_user_id, nickname: row.desired_nickname });
      completed += db.prepare(`DELETE FROM profile_im_sync WHERE user_id=? AND revision=?`)
        .run(row.user_id, row.revision).changes;
    } catch (error) {
      const safeError = /^HTTP [0-9]{3}$|^IM error [0-9]+$/.test(error.message) ?
        error.message : 'IM sync unavailable';
      db.prepare(`UPDATE profile_im_sync SET attempts=attempts+1,last_error=?
        WHERE user_id=? AND revision=?`).run(safeError, row.user_id, row.revision);
    }
  }
  return { pending: rows.length - completed, completed };
}

module.exports = { createTencentProfileWriter, syncPendingProfileActions };
