'use strict';

const assert = require('node:assert/strict');
const { createHmac } = require('node:crypto');
const test = require('node:test');
const { createAsrRealtimeUrlIssuer } = require('./asr-realtime-url');

const env = { MAICHAT_ASR_APP_ID: '1234567890', MAICHAT_ASR_SECRET_ID: 'AKID12345678',
  MAICHAT_ASR_SECRET_KEY: 'test-only-server-secret-key' };

test('signs a five-minute AAC ASR URL with unique voice ID and no long-lived key', () => {
  let serial = 0;
  const issue = createAsrRealtimeUrlIssuer(env, {
    currentTime: () => 1_780_000_000_000,
    newVoiceId: () => `00000000-0000-4000-8000-${String(++serial).padStart(12, '0')}`,
    newNonce: () => 123456789
  });
  const first = issue();
  const second = issue();
  assert.notEqual(first.voice_id, second.voice_id);
  assert.equal(first.expires_at, 1_780_000_300);
  const url = new URL(first.url);
  assert.equal(url.protocol, 'wss:');
  assert.equal(url.hostname, 'asr.cloud.tencent.com');
  assert.equal(url.pathname, '/asr/v2/1234567890');
  assert.equal(url.searchParams.get('engine_model_type'), '16k_zh');
  assert.equal(url.searchParams.get('voice_format'), '16');
  assert.equal(url.searchParams.get('needvad'), '1');
  const canonical = [...url.searchParams.entries()]
    .filter(([key]) => key !== 'signature')
    .sort(([left], [right]) => left.localeCompare(right))
    .map(([key, value]) => `${key}=${value}`).join('&');
  const expected = createHmac('sha1', env.MAICHAT_ASR_SECRET_KEY)
    .update(`asr.cloud.tencent.com${url.pathname}?${canonical}`).digest('base64');
  assert.equal(url.searchParams.get('signature'), expected);
  assert.ok(!first.url.includes(env.MAICHAT_ASR_SECRET_KEY));
});

test('requires complete server-side signing credentials', () => {
  assert.equal(createAsrRealtimeUrlIssuer({}), null);
  assert.throws(() => createAsrRealtimeUrlIssuer({ ...env,
    MAICHAT_ASR_SECRET_KEY: '' }), /credentials are invalid/);
});
