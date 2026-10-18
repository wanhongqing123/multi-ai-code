// Apply MaiChat's accepted friendship state to Tencent IM. No client credentials are used.
'use strict';

const { randomInt } = require('node:crypto');
const { createUserSig } = require('./usersig');

function createTencentFriendWriter({ sdkAppId, imSecret, adminUser = 'administrator',
  origin = 'https://console.tim.qq.com', fetchImpl = fetch }) {
  if (!Number.isSafeInteger(sdkAppId) || sdkAppId <= 0 ||
      typeof imSecret !== 'string' || imSecret.length < 32 ||
      !/^[A-Za-z0-9_-]{1,32}$/.test(adminUser) || !origin.startsWith('https://'))
    throw new Error('IM friend writer configuration is invalid');
  async function perform(endpoint, body) {
    const url = new URL(endpoint, origin);
    url.search = new URLSearchParams({ sdkappid: String(sdkAppId), identifier: adminUser,
      usersig: createUserSig(sdkAppId, adminUser, imSecret, Math.floor(Date.now() / 1000)),
      random: String(randomInt(1, 0xFFFFFFFF)), contenttype: 'json' }).toString();
    const response = await fetchImpl(url, { method: 'POST', redirect: 'error',
      headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body),
      signal: AbortSignal.timeout(10_000) });
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    const text = await response.text();
    if (text.length > 64 * 1024) throw new Error('IM response is too large');
    let result;
    try { result = JSON.parse(text); } catch { throw new Error('Invalid IM response'); }
    if (result.ActionStatus !== 'OK' || result.ErrorCode !== 0)
      throw new Error(`IM error ${result.ErrorCode ?? 'unknown'}`);
    return result;
  }
  return async ({ action, from, to }) => {
    if (!['add', 'delete'].includes(action) ||
        ![from, to].every(id => typeof id === 'string' &&
          /^[A-Za-z0-9_-]{1,32}$/.test(id)))
      throw new Error('Invalid IM friend operation');
    const endpoint = action === 'add' ? '/v4/sns/friend_add' : '/v4/sns/friend_delete';
    const body = action === 'add' ? { From_Account: from,
      AddFriendItem: [{ To_Account: to, AddSource: 'AddSource_Type_MaiChat' }],
      AddType: 'Add_Type_Both', ForceAddFlags: 1 } :
      { From_Account: from, To_Account: [to], DeleteType: 'Delete_Type_Both' };
    const result = await perform(endpoint, body);
    const item = Array.isArray(result.ResultItem) && result.ResultItem.length === 1 ?
      result.ResultItem[0] : null;
    if (!item || item.To_Account !== to) throw new Error('Invalid IM friend response');
    if (item.ResultCode === 0 || (action === 'delete' && item.ResultCode === 31704)) return;
    if (action === 'add' && item.ResultCode === 30015) {
      const checked = await perform('/v4/sns/friend_check', { From_Account: from,
        To_Account: [to], CheckType: 'CheckResult_Type_Both' });
      const relation = Array.isArray(checked.InfoItem) && checked.InfoItem.length === 1 ?
        checked.InfoItem[0] : null;
      if (relation?.To_Account === to && relation.ResultCode === 0 &&
          relation.Relation === 'CheckResult_Type_BothWay') return;
    }
    throw new Error(`IM error ${item.ResultCode}`);
  };
}

async function syncPendingFriendActions(db, writer, limit = 50) {
  const rows = db.prepare(`SELECT friend_im_sync.*,left_user.im_user_id AS left_im_id,
    right_user.im_user_id AS right_im_id FROM friend_im_sync
    JOIN users AS left_user ON left_user.id=friend_im_sync.left_user_id
    JOIN users AS right_user ON right_user.id=friend_im_sync.right_user_id
    ORDER BY updated_at LIMIT ?`).all(limit);
  let completed = 0;
  for (const row of rows) {
    try {
      await writer({ action: row.desired, from: row.left_im_id, to: row.right_im_id });
      db.prepare(`DELETE FROM friend_im_sync WHERE left_user_id=? AND right_user_id=?
        AND revision=?`).run(row.left_user_id, row.right_user_id, row.revision);
      completed++;
    } catch (error) {
      const safeError = /^HTTP [0-9]{3}$|^IM error [0-9]+$/.test(error.message) ?
        error.message : 'IM sync unavailable';
      db.prepare(`UPDATE friend_im_sync SET attempts=attempts+1,last_error=?
        WHERE left_user_id=? AND right_user_id=? AND revision=?`)
        .run(safeError, row.left_user_id, row.right_user_id, row.revision);
    }
  }
  return { pending: rows.length - completed, completed };
}

module.exports = { createTencentFriendWriter, syncPendingFriendActions };
