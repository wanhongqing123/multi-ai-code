// Sign short-lived Tencent ASR WebSocket URLs; audio never passes through this service.
'use strict';

const { createHmac, randomInt, randomUUID } = require('node:crypto');

const URL_LIFETIME_SECONDS = 300;

function createAsrRealtimeUrlIssuer(env, options = {}) {
  const appId = env.MAICHAT_ASR_APP_ID || '';
  const secretId = env.MAICHAT_ASR_SECRET_ID || '';
  const secretKey = env.MAICHAT_ASR_SECRET_KEY || '';
  if (!appId && !secretId && !secretKey) return null;
  if (!/^[0-9]{5,20}$/.test(appId) || !/^[A-Za-z0-9]{8,128}$/.test(secretId) ||
      secretKey.length < 16 || secretKey.length > 256)
    throw new Error('Tencent ASR signing credentials are invalid');
  const currentTime = options.currentTime || Date.now;
  const newVoiceId = options.newVoiceId || randomUUID;
  const newNonce = options.newNonce || (() => randomInt(1, 2_147_483_647));

  return function issueAsrRealtimeUrl() {
    const voiceId = newVoiceId();
    const nonce = newNonce();
    if (!/^[a-f0-9-]{36}$/.test(voiceId) ||
        !Number.isSafeInteger(nonce) || nonce < 1 || nonce > 2_147_483_646)
      throw new Error('Invalid ASR request identity');
    const timestamp = Math.floor(currentTime() / 1000);
    const expiresAt = timestamp + URL_LIFETIME_SECONDS;
    const parameters = {
      engine_model_type: '16k_zh', expired: String(expiresAt), needvad: '1',
      nonce: String(nonce), secretid: secretId, timestamp: String(timestamp),
      voice_format: '16', voice_id: voiceId
    };
    const query = Object.keys(parameters).sort()
      .map(key => `${key}=${parameters[key]}`).join('&');
    const endpoint = `asr.cloud.tencent.com/asr/v2/${appId}`;
    const signature = createHmac('sha1', secretKey)
      .update(`${endpoint}?${query}`).digest('base64');
    return { voice_id: voiceId,
      url: `wss://${endpoint}?${query}&signature=${encodeURIComponent(signature)}`,
      expires_at: expiresAt };
  };
}

module.exports = { createAsrRealtimeUrlIssuer };
