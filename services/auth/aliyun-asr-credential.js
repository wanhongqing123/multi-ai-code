'use strict';

const REFRESH_BEFORE_SECONDS = 90;

function validAliyunAsrCredential(value, timestamp) {
  return value && typeof value.app_key === 'string' &&
    /^[!-~]{1,256}$/.test(value.app_key) && typeof value.token === 'string' &&
    /^[!-~]{1,8192}$/.test(value.token) && Number.isSafeInteger(value.expires_at) &&
    value.expires_at > timestamp + REFRESH_BEFORE_SECONDS;
}

function createAliyunAsrCredentialIssuer(env, options = {}) {
  const appKey = env.MAICHAT_NLS_APP_KEY || '';
  const accessKeyId = env.MAICHAT_NLS_ACCESS_KEY_ID || '';
  const accessKeySecret = env.MAICHAT_NLS_ACCESS_KEY_SECRET || '';
  // NLS configuration is independent from Tencent and the SMS/OSS identities.
  // An incomplete or invalid optional configuration disables only this feature.
  if (typeof appKey !== 'string' || !/^[!-~]{1,256}$/.test(appKey) ||
      typeof accessKeyId !== 'string' || !/^[!-~]{1,1024}$/.test(accessKeyId) ||
      typeof accessKeySecret !== 'string' || !/^[!-~]{1,1024}$/.test(accessKeySecret))
    return null;
  const currentTime = options.currentTime || Date.now;
  // Official RPCClient owns POP signing; the account service never relays audio.
  // https://help.aliyun.com/zh/isi/user-guide/obtain-an-access-token
  const createClient = options.createClient || (config => {
    const { RPCClient } = require('@alicloud/pop-core');
    return new RPCClient(config);
  });
  const client = createClient({ accessKeyId, accessKeySecret,
    endpoint: 'https://nls-meta.cn-shanghai.aliyuncs.com', apiVersion: '2019-02-28' });
  let cached = null;
  let pending = null;

  return async function issueAliyunAsrCredential() {
    if (cached && validAliyunAsrCredential(cached, Math.floor(currentTime() / 1000)))
      return { ...cached };
    // Refreshes share one request across accounts. Drop stale state before the
    // request, so a provider failure cannot revive an expired or near-expiry token.
    cached = null;
    if (!pending) {
      pending = (async () => {
        const reply = await client.request('CreateToken', {}, { method: 'POST', timeout: 8000 });
        const issued = { app_key: appKey, token: reply?.Token?.Id,
          expires_at: reply?.Token?.ExpireTime };
        // ExpireTime is an upstream Unix timestamp, not a fixed token duration.
        // Never substitute the Tencent certificate lifetime or disclose an AK.
        if (!validAliyunAsrCredential(issued, Math.floor(currentTime() / 1000)) ||
            issued.token === accessKeyId || issued.token === accessKeySecret)
          throw new Error('Aliyun NLS returned an invalid speech token');
        cached = Object.freeze(issued);
        return cached;
      })();
    }
    const issuance = pending;
    try { return { ...await issuance }; }
    finally { if (pending === issuance) pending = null; }
  };
}

module.exports = { createAliyunAsrCredentialIssuer, validAliyunAsrCredential };
