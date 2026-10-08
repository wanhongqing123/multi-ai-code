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

test('signs image upload and forwards virtual asset actions to Ark', async () => {
  const calls = [];
  const handle = createHandler({
    env: { ...env, VOLC_ACCESS_KEY_ID: 'volc-ak', VOLC_SECRET_ACCESS_KEY: 'volc-sk',
      VOLC_ARK_PROJECT_NAME: 'my-project' },
    newID: () => 'image-uuid', currentTime: () => new Date('2026-10-08T00:00:00Z'),
    makeClient: () => ({ signatureUrlV4: async (method, ttl, options, key) => {
      if (method === 'GET') assert.equal(ttl, 7 * 24 * 60 * 60);
      return `https://example.oss-cn-hangzhou.aliyuncs.com/${key}?signed=${method}`;
    } }),
    callArk: async (action, payload, credentials) => {
      calls.push({ action, payload, credentials });
      if (action === 'CreateAssetGroup') return { Result: { Id: 'group-1234567890' } };
      if (action === 'CreateAsset') return { Result: { Id: 'asset-1234567890' } };
      return { Result: { Id: 'asset-1234567890', Status: 'Active' } };
    }
  });
  const signed = await handle(request({ filename: 'portrait.JPG', size_bytes: 100 }));
  assert.equal(signed.statusCode, 200);
  assert.equal(JSON.parse(signed.body).object_key,
    'ark-asset-inputs/2026-10-08/image-uuid.jpg');
  assert.equal(JSON.parse(signed.body).upload_headers['Content-Type'], 'image/jpeg');
  const arkRequest = body => ({ ...request(body), rawPath: '/ark-assets' });
  const group = await handle(arkRequest({ action: 'create_group', name: 'figure' }));
  assert.equal(JSON.parse(group.body).result.Id, 'group-1234567890');
  const asset = await handle(arkRequest({ action: 'create_asset',
    group_id: 'group-1234567890', name: 'portrait',
    url: 'https://example.oss-cn-hangzhou.aliyuncs.com/photo.jpg?sig=test' }));
  assert.equal(JSON.parse(asset.body).result.Id, 'asset-1234567890');
  const state = await handle(arkRequest({ action: 'get_asset', asset_id: 'asset-1234567890' }));
  assert.equal(JSON.parse(state.body).result.Status, 'Active');
  assert.equal(calls[0].payload.GroupType, 'AIGC');
  assert.equal(calls[0].payload.ProjectName, 'my-project');
  assert.equal(calls[1].payload.AssetType, 'Image');
  assert.equal(calls[1].payload.ProjectName, 'my-project');
  assert.equal(calls[1].credentials.secretKey, 'volc-sk');
  assert.ok(!asset.body.includes('volc-sk'));
});

test('rejects invalid Ark requests and preserves async provider failure', async () => {
  const arkEnv = { ...env, VOLC_ACCESS_KEY_ID: 'ak', VOLC_SECRET_ACCESS_KEY: 'sk' };
  const handle = createHandler({ env: arkEnv, callArk: async () => ({
    Result: { Id: 'asset-1234567890', Status: 'Failed', Error: {
      Code: 'ModerationFailed', Message: 'Input rejected' } }
  }) });
  const arkRequest = body => ({ ...request(body), rawPath: '/ark-assets' });
  assert.equal((await handle(arkRequest({ action: 'create_asset',
    group_id: 'group-1234567890', url: 'http://localhost/photo.jpg' }))).statusCode, 400);
  const failed = await handle(arkRequest({ action: 'get_asset', asset_id: 'asset-1234567890' }));
  assert.equal(JSON.parse(failed.body).result.Status, 'Failed');
  assert.equal(JSON.parse(failed.body).result.Error.Code, 'ModerationFailed');
});

test('accepts direct asset URL without an OSS bucket', async () => {
  const arkOnly = {
    MAICHAT_MEDIA_SERVICE_TOKEN: 'abcdef0123456789abcdef0123456789',
    VOLC_ACCESS_KEY_ID: 'ark-ak', VOLC_SECRET_ACCESS_KEY: 'ark-sk'
  };
  const handle = createHandler({ env: arkOnly, callArk: async (action, payload) => {
    assert.equal(action, 'CreateAsset');
    assert.equal(payload.URL, 'https://images.example.com/portrait.png');
    return { Result: { Id: 'asset-1234567890' } };
  } });
  const direct = { rawPath: '/ark-assets', requestContext: { http: { method: 'POST' } },
    headers: { Authorization: `Bearer ${arkOnly.MAICHAT_MEDIA_SERVICE_TOKEN}` },
    body: JSON.stringify({ action: 'create_asset', group_id: 'group-1234567890',
      url: 'https://images.example.com/portrait.png' }) };
  assert.equal((await handle(direct)).statusCode, 200);
  assert.equal((await handle({ ...direct, rawPath: '/sign-upload' })).statusCode, 503);
});

test('returns only requested static API keys to an authenticated client', async () => {
  const credentialEnv = { ...env,
    MAICHAT_ARK_API_KEY: 'ark-project-key',
    MAICHAT_GLM_API_KEY: 'glm-key',
    MAICHAT_DEEPSEEK_API_KEY: 'deepseek-key',
    MAICHAT_WAN_API_KEY: 'wan-key',
    MAICHAT_WAN_WORKSPACE_ID: 'ws-example',
    MAICHAT_KLING_API_KEY: 'must-not-be-returned',
    MAICHAT_MINIMAX_API_KEY: 'minimax-key',
    VOLC_ACCESS_KEY_ID: 'volc-access-id',
    VOLC_SECRET_ACCESS_KEY: 'volc-access-secret' };
  const handle = createHandler({ env: credentialEnv });
  const call = (body, token = env.MAICHAT_OSS_SIGNER_TOKEN) =>
    ({ ...request(body, token), rawPath: '/credentials' });
  const status = await handle(call({ action: 'status' }));
  assert.equal(status.statusCode, 200);
  assert.deepEqual(JSON.parse(status.body), {
    configured: ['ark', 'glm', 'deepseek', 'wan', 'minimax'],
    assets_configured: true, storage_configured: true
  });
  const fetched = await handle(call({ action: 'fetch',
    providers: ['ark', 'glm', 'deepseek', 'wan', 'minimax'] }));
  assert.equal(fetched.statusCode, 200);
  assert.deepEqual(JSON.parse(fetched.body), {
    api_keys: {
      ark: 'ark-project-key', glm: 'glm-key', deepseek: 'deepseek-key',
      wan: 'wan-key', minimax: 'minimax-key'
    },
    wan_workspace_id: 'ws-example'
  });
  assert.equal(fetched.headers['Cache-Control'], 'no-store');
  assert.equal(fetched.headers.Pragma, 'no-cache');
  assert.ok(!fetched.body.includes('volc-access-secret'));
  assert.ok(!fetched.body.includes('must-not-be-returned'));
  assert.equal((await handle(call({ action: 'fetch', providers: ['glm'] }, 'wrong'))).statusCode, 401);
  assert.equal((await handle(call({ action: 'fetch', providers: ['access_key'] }))).statusCode, 400);
  assert.equal((await handle(call({ action: 'fetch', providers: ['kling'] }))).statusCode, 400);
  assert.equal((await handle(call({ action: 'fetch', providers: ['volc_access'] }))).statusCode, 400);
  assert.equal((await handle(call({ action: 'fetch', providers: ['glm', 'glm'] }))).statusCode, 400);
});
