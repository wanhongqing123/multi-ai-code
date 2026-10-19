// Validate that a manually chosen test ID is unused by Tencent IM before binding it.
'use strict';

const { randomInt } = require('node:crypto');
const { createUserSig } = require('./usersig');

function createTencentAccountProvisioner({ sdkAppId, imSecret, adminUser = 'administrator',
  origin = 'https://console.tim.qq.com', fetchImpl = fetch }) {
  if (!Number.isSafeInteger(sdkAppId) || sdkAppId <= 0 ||
      typeof imSecret !== 'string' || imSecret.length < 32 ||
      !/^[A-Za-z0-9_-]{1,32}$/.test(adminUser) || !origin.startsWith('https://'))
    throw new Error('IM account provision configuration is invalid');
  async function call(endpoint, body) {
    const url = new URL(endpoint, origin);
    url.search = new URLSearchParams({ sdkappid: String(sdkAppId), identifier: adminUser,
      usersig: createUserSig(sdkAppId, adminUser, imSecret, Math.floor(Date.now() / 1000)),
      random: String(randomInt(1, 0xFFFFFFFF)), contenttype: 'json' }).toString();
    const response = await fetchImpl(url, { method: 'POST', redirect: 'error',
      headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body),
      signal: AbortSignal.timeout(10_000) });
    if (!response.ok) throw new Error(`IM HTTP ${response.status}`);
    const text = await response.text();
    if (text.length > 64 * 1024) throw new Error('IM response is too large');
    let result;
    try { result = JSON.parse(text); } catch { throw new Error('Invalid IM response'); }
    if (result.ActionStatus !== 'OK' || result.ErrorCode !== 0)
      throw new Error(`IM error ${result.ErrorCode ?? 'unknown'}`);
    return result;
  }
  async function checkStatus(accountId) {
    if (typeof accountId !== 'string' || !/^[a-z][a-z0-9_-]{2,31}$/.test(accountId))
      throw new Error('Invalid test IM ID');
    const checked = await call('/v4/im_open_login_svc/account_check',
      { CheckItem: [{ UserID: accountId }] });
    const item = Array.isArray(checked.ResultItem) && checked.ResultItem.length === 1 ?
      checked.ResultItem[0] : null;
    if (!item || item.UserID !== accountId || item.ResultCode !== 0)
      throw new Error('Invalid IM account check');
    if (!['Imported', 'NotImported'].includes(item.AccountStatus))
      throw new Error('Unknown IM account status');
    return item.AccountStatus;
  }
  const provision = async accountId => {
    if (await checkStatus(accountId) === 'Imported') return false;
    await call('/v4/im_open_login_svc/account_import', { UserID: accountId });
    return true;
  };
  provision.checkStatus = checkStatus;
  return provision;
}

module.exports = { createTencentAccountProvisioner };
