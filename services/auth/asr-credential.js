'use strict';

const CREDENTIAL_LIFETIME_SECONDS = 1800;
const REFRESH_BEFORE_SECONDS = 90;

// RealtimeRecognition only supports operation-level CAM authorization, so its
// resource must be '*'. Do not grant unrelated ASR APIs to mobile clients.
// https://cloud.tencent.com/document/product/1093/48422
// https://cloud.tencent.com/document/product/1093/48423
const ASR_POLICY = JSON.stringify({ version: '2.0', statement: [{
  effect: 'allow', action: ['name/asr:RealtimeRecognition'], resource: ['*']
}] });

function validAsrCredential(value, timestamp) {
  // STS temporary IDs are opaque strings, unlike permanent AKID values. The
  // official Credentials model permits up to 1024 bytes, including symbols.
  // Restrict whitespace/control bytes without assuming an alphanumeric ID.
  // https://cloud.tencent.com/document/product/1312/48198
  return value && typeof value.app_id === 'string' &&
    /^[0-9]{5,20}$/.test(value.app_id) &&
    typeof value.secret_id === 'string' && /^[!-~]{8,1024}$/.test(value.secret_id) &&
    typeof value.secret_key === 'string' && /^[!-~]{16,256}$/.test(value.secret_key) &&
    typeof value.token === 'string' && /^[!-~]{16,8192}$/.test(value.token) &&
    Number.isSafeInteger(value.expires_at) &&
    value.expires_at > timestamp + REFRESH_BEFORE_SECONDS &&
    value.expires_at <= timestamp + CREDENTIAL_LIFETIME_SECONDS + 60;
}

function createAsrCredentialIssuer(env, options = {}) {
  const appId = env.MAICHAT_ASR_APP_ID || '';
  const secretId = env.MAICHAT_ASR_SECRET_ID || '';
  const secretKey = env.MAICHAT_ASR_SECRET_KEY || '';
  if (!appId && !secretId && !secretKey) return null;
  if (!/^[0-9]{5,20}$/.test(appId) || !/^[A-Za-z0-9]{8,128}$/.test(secretId) ||
      secretKey.length < 16 || secretKey.length > 256)
    throw new Error('Tencent ASR signing credentials are invalid');
  const currentTime = options.currentTime || Date.now;
  // The official SDK owns Tencent API signing and HTTPS transport. Only its
  // control-plane STS call runs here; clients send audio directly to Tencent.
  const createClient = options.createClient || (config => {
    const { sts } = require('tencentcloud-sdk-nodejs-sts');
    return new sts.v20180813.Client(config);
  });
  const client = createClient({ credential: { secretId, secretKey },
    region: 'ap-guangzhou', profile: { httpProfile: {
      endpoint: 'sts.tencentcloudapi.com', reqTimeout: 8
    } } });
  let cached = null;
  let pending = null;

  return async function issueAsrCredential() {
    const timestamp = Math.floor(currentTime() / 1000);
    if (cached && validAsrCredential(cached, timestamp)) return { ...cached };
    // Drop stale state before refreshing. A failed refresh must never return an
    // expired certificate, and concurrent accounts share one STS request.
    cached = null;
    if (!pending) {
      pending = (async () => {
        // STS explicitly requires URL encoding of the Policy field even when
        // the SDK serializes the enclosing API request as JSON.
        // https://cloud.tencent.com/document/product/1312/48195
        const reply = await client.GetFederationToken({ Name: 'maichatasr',
          Policy: encodeURIComponent(ASR_POLICY),
          DurationSeconds: CREDENTIAL_LIFETIME_SECONDS });
        const issued = { app_id: appId, secret_id: reply?.Credentials?.TmpSecretId,
          secret_key: reply?.Credentials?.TmpSecretKey, token: reply?.Credentials?.Token,
          expires_at: reply?.ExpiredTime };
        if (!validAsrCredential(issued, Math.floor(currentTime() / 1000)) ||
            issued.secret_id === secretId || issued.secret_key === secretKey)
          throw new Error('Tencent STS returned an invalid ASR credential');
        cached = Object.freeze(issued);
        return cached;
      })();
    }
    const issuance = pending;
    try { return { ...await issuance }; }
    finally { if (pending === issuance) pending = null; }
  };
}

module.exports = { createAsrCredentialIssuer, validAsrCredential };
