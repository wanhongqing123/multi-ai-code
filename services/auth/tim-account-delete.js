// Delete a verified pending-deletion IM account through Tencent's administrator API.
'use strict';

const { randomInt } = require('node:crypto');
const { createUserSig } = require('./usersig');

function createTencentAccountDeleter({ sdkAppId, imSecret, adminUser = 'administrator',
  origin = 'https://console.tim.qq.com', fetchImpl = fetch }) {
  if (!Number.isSafeInteger(sdkAppId) || sdkAppId <= 0 ||
      typeof imSecret !== 'string' || imSecret.length < 32 ||
      !/^[A-Za-z0-9_-]{1,32}$/.test(adminUser) || !origin.startsWith('https://'))
    throw new Error('IM account deletion configuration is invalid');
  return async function deleteAccount(imUserId) {
    if (typeof imUserId !== 'string' || !/^[A-Za-z0-9_-]{1,64}$/.test(imUserId))
      throw new Error('Invalid IM account ID');
    const url = new URL('/v4/im_open_login_svc/account_delete', origin);
    url.search = new URLSearchParams({ sdkappid: String(sdkAppId), identifier: adminUser,
      usersig: createUserSig(sdkAppId, adminUser, imSecret, Math.floor(Date.now() / 1000)),
      random: String(randomInt(1, 0xFFFFFFFF)), contenttype: 'json' }).toString();
    const response = await fetchImpl(url, { method: 'POST', redirect: 'error',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ DeleteItem: [{ UserID: imUserId }] }),
      signal: AbortSignal.timeout(10_000) });
    if (!response.ok) throw new Error(`IM HTTP ${response.status}`);
    const text = await response.text();
    if (text.length > 64 * 1024) throw new Error('IM response is too large');
    let result;
    try { result = JSON.parse(text); } catch { throw new Error('Invalid IM response'); }
    if (result.ActionStatus !== 'OK' || result.ErrorCode !== 0)
      throw new Error(`IM error ${result.ErrorCode ?? 'unknown'}`);
    const item = Array.isArray(result.ResultItem) && result.ResultItem.length === 1
      ? result.ResultItem[0] : null;
    if (!item || item.UserID !== imUserId || ![0, 70107].includes(item.ResultCode))
      throw new Error(`IM account deletion failed: ${item?.ResultCode ?? 'unknown'}`);
    return { user_id: imUserId, result_code: item.ResultCode };
  };
}

module.exports = { createTencentAccountDeleter };
