'use strict';

const { createHash, createHmac } = require('node:crypto');

const ARK_HOST = 'ark.cn-beijing.volcengineapi.com';
const ARK_REGION = 'cn-beijing';
const ARK_SERVICE = 'ark';
const ARK_VERSION = '2024-01-01';

function sha256(value) {
  return createHash('sha256').update(value).digest('hex');
}

function hmac(key, value) {
  return createHmac('sha256', key).update(value).digest();
}

function signArkRequest(action, body, credentials, now = new Date()) {
  const timestamp = now.toISOString().replace(/[-:]|\.\d{3}/g, '');
  const day = timestamp.slice(0, 8);
  const scope = `${day}/${ARK_REGION}/${ARK_SERVICE}/request`;
  const bodyHash = sha256(body);
  const query = `Action=${encodeURIComponent(action)}&Version=${ARK_VERSION}`;
  const signedHeaders = 'host;x-content-sha256;x-date';
  const canonicalHeaders = `host:${ARK_HOST}\nx-content-sha256:${bodyHash}\nx-date:${timestamp}\n`;
  const canonicalRequest = ['POST', '/', query, canonicalHeaders, signedHeaders, bodyHash]
    .join('\n');
  const stringToSign = ['HMAC-SHA256', timestamp, scope, sha256(canonicalRequest)]
    .join('\n');
  const signingKey = hmac(hmac(hmac(hmac(credentials.secretKey, day), ARK_REGION),
    ARK_SERVICE), 'request');
  const signature = hmac(signingKey, stringToSign).toString('hex');
  return {
    url: `https://${ARK_HOST}/?${query}`,
    headers: {
      Host: ARK_HOST,
      'Content-Type': 'application/json',
      'X-Date': timestamp,
      'X-Content-Sha256': bodyHash,
      Authorization: `HMAC-SHA256 Credential=${credentials.accessKeyId}/${scope}, ` +
        `SignedHeaders=${signedHeaders}, Signature=${signature}`
    }
  };
}

async function callArk(action, payload, credentials, fetcher = fetch) {
  const body = JSON.stringify(payload);
  const signed = signArkRequest(action, body, credentials);
  const response = await fetcher(signed.url, {
    method: 'POST', headers: signed.headers, body,
    signal: AbortSignal.timeout(30_000)
  });
  const raw = await response.text();
  if (Buffer.byteLength(raw) > 1_000_000) throw new Error('Ark response is too large');
  return JSON.parse(raw);
}

exports.callArk = callArk;
exports.signArkRequest = signArkRequest;
