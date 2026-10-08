'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const { createHandler } = require('./index');

const env = {
  MAICHAT_OSS_SIGNER_TOKEN: '0123456789abcdef0123456789abcdef',
  MAICHAT_OSS_BUCKET: 'example-private-bucket',
  MAICHAT_OSS_REGION: 'oss-cn-hangzhou',
  ALIBABA_CLOUD_ACCESS_KEY_ID: 'test-id',
  ALIBABA_CLOUD_ACCESS_KEY_SECRET: 'test-secret'
};

function request(body, token = env.MAICHAT_OSS_SIGNER_TOKEN) {
  return {
    rawPath: '/sign-upload',
    requestContext: { http: { method: 'POST' } },
    headers: { Authorization: `Bearer ${token}` },
    body: JSON.stringify(body)
  };
}

test('signs a private video PUT and temporary GET for a random object key', async () => {
  const calls = [];
  const handle = createHandler({
    env,
    newID: () => '1234-unique',
    currentTime: () => new Date('2026-10-08T00:00:00Z'),
    makeClient: configuration => {
      assert.equal(configuration.bucket, env.MAICHAT_OSS_BUCKET);
      assert.equal(configuration.authorizationV4, true);
      assert.equal(configuration.secure, true);
      return {
        signatureUrlV4: async (...args) => {
          calls.push(args);
          return `https://example.oss-cn-hangzhou.aliyuncs.com/${args[3]}?signed=${args[0]}`;
        }
      };
    }
  });
  const result = await handle(request({ filename: 'clip.MOV', size_bytes: 20_000_000 }));
  assert.equal(result.statusCode, 200);
  const body = JSON.parse(result.body);
  assert.equal(body.object_key, 'seedance-inputs/2026-10-08/1234-unique.mov');
  assert.equal(body.upload_headers['Content-Type'], 'video/quicktime');
  assert.deepEqual(calls.map(call => [call[0], call[1], call[3]]), [
    ['PUT', 900, body.object_key], ['GET', 86400, body.object_key]
  ]);
  assert.equal(calls[0][2].headers['Content-Type'], 'video/quicktime');
  assert.ok(!result.body.includes('test-secret'));
});

test('rejects unauthorized and oversized requests before signing', async () => {
  let created = false;
  const handle = createHandler({ env, makeClient: () => { created = true; } });
  assert.equal((await handle(request({ filename: 'clip.mp4', size_bytes: 1 }, 'wrong'))).statusCode, 401);
  assert.equal((await handle(request({ filename: 'clip.mp4', size_bytes: 200_000_001 }))).statusCode, 400);
  assert.equal((await handle(request({ filename: 'clip.exe', size_bytes: 12 }))).statusCode, 400);
  assert.equal(created, false);
});

test('fails closed when credentials or route are missing', async () => {
  const missing = createHandler({ env: { ...env, ALIBABA_CLOUD_ACCESS_KEY_SECRET: '' } });
  assert.equal((await missing(request({ filename: 'clip.mp4', size_bytes: 12 }))).statusCode, 503);
  const active = createHandler({ env });
  const wrongRoute = request({ filename: 'clip.mp4', size_bytes: 12 });
  wrongRoute.rawPath = '/other';
  assert.equal((await active(wrongRoute)).statusCode, 404);
});
