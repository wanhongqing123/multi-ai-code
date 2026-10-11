'use strict';

const { createHmac } = require('node:crypto');
const { deflateSync } = require('node:zlib');

// 与三端原有的 UserSig v2 格式一致，签名密钥只能由服务端持有。
function createUserSig(sdkAppId, userId, secret, issuedAt, lifetimeSeconds = 7 * 24 * 60 * 60) {
  if (!Number.isSafeInteger(sdkAppId) || sdkAppId <= 0 ||
      !/^[A-Za-z0-9_-]{1,32}$/.test(userId) ||
      typeof secret !== 'string' || secret.length < 32 ||
      !Number.isSafeInteger(issuedAt) || issuedAt < 1 ||
      !Number.isSafeInteger(lifetimeSeconds) || lifetimeSeconds < 60)
    throw new Error('Invalid IM signature configuration');
  const input = `TLS.identifier:${userId}\nTLS.sdkappid:${sdkAppId}\n` +
    `TLS.time:${issuedAt}\nTLS.expire:${lifetimeSeconds}\n`;
  const signature = createHmac('sha256', secret).update(input).digest('base64');
  const payload = JSON.stringify({
    'TLS.ver': '2.0', 'TLS.identifier': userId, 'TLS.sdkappid': sdkAppId,
    'TLS.expire': lifetimeSeconds, 'TLS.time': issuedAt, 'TLS.sig': signature
  });
  return deflateSync(Buffer.from(payload)).toString('base64')
    .replace(/\+/g, '*').replace(/\//g, '-').replace(/=/g, '_');
}

module.exports = { createUserSig };
