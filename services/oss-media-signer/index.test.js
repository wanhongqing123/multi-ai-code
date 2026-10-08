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
  assert.equal((await handle(request({ filename: 'photo.jpg', size_bytes: 30_000_001 }))).statusCode, 400);
  assert.equal(created, false);
});

test('signs direct JPEG uploads for private Ark input storage', async () => {
  const calls = [];
  const handle = createHandler({ env, newID: () => 'image-id',
    currentTime: () => new Date('2026-10-08T00:00:00Z'),
    makeClient: () => ({ signatureUrlV4: async (...args) => {
      calls.push(args);
      return `https://example.oss-cn-hangzhou.aliyuncs.com/${args[3]}?signed=yes`;
    } }) });
  const result = await handle(request({ filename: 'photo.JPG', size_bytes: 1_000_000 }));
  assert.equal(result.statusCode, 200);
  const body = JSON.parse(result.body);
  assert.equal(body.object_key, 'ark-asset-inputs/2026-10-08/image-id.jpg');
  assert.equal(body.upload_headers['Content-Type'], 'image/jpeg');
  assert.deepEqual(calls.map(call => call[0]), ['PUT', 'GET']);
});

test('fails closed when credentials or route are missing', async () => {
  const missing = createHandler({ env: { ...env, ALIBABA_CLOUD_ACCESS_KEY_SECRET: '' } });
  assert.equal((await missing(request({ filename: 'clip.mp4', size_bytes: 12 }))).statusCode, 503);
  const active = createHandler({ env });
  const wrongRoute = request({ filename: 'clip.mp4', size_bytes: 12 });
  wrongRoute.rawPath = '/other';
  assert.equal((await active(wrongRoute)).statusCode, 404);
});

test('uses the Ark signing key only for validated asset actions', async () => {
  const calls = [];
  const assetEnv = { ...env, VOLC_ACCESS_KEY_ID: 'volc-ak',
    VOLC_SECRET_ACCESS_KEY: 'volc-sk', VOLC_ARK_PROJECT_NAME: 'my-project' };
  const handle = createHandler({ env: assetEnv, callArk: async (action, payload, credentials) => {
    calls.push({ action, payload, credentials });
    if (action === 'CreateAssetGroup') return { Result: { Id: 'group-1234567890' } };
    if (action === 'CreateAsset') return { Result: { Id: 'asset-1234567890' } };
    return { Result: { Id: 'asset-1234567890', Status: 'Active' } };
  } });
  const assetRequest = body => ({ ...request(body), rawPath: '/ark-assets' });
  const group = await handle(assetRequest({ action: 'create_group', name: 'portrait' }));
  assert.equal(JSON.parse(group.body).result.Id, 'group-1234567890');
  const asset = await handle(assetRequest({ action: 'create_asset',
    group_id: 'group-1234567890', name: 'portrait',
    url: 'https://images.example.com/portrait.jpg' }));
  assert.equal(JSON.parse(asset.body).result.Id, 'asset-1234567890');
  const state = await handle(assetRequest({ action: 'get_asset', asset_id: 'asset-1234567890' }));
  assert.equal(JSON.parse(state.body).result.Status, 'Active');
  assert.equal(calls[0].payload.GroupType, 'AIGC');
  assert.equal(calls[0].payload.ProjectName, 'my-project');
  assert.equal(calls[1].payload.AssetType, 'Image');
  assert.equal(calls[1].credentials.secretKey, 'volc-sk');
  assert.ok(!asset.body.includes('volc-sk'));
  const invalid = await handle(assetRequest({ action: 'create_asset',
    group_id: 'group-1234567890', url: 'http://localhost/photo.jpg' }));
  assert.equal(invalid.statusCode, 400);
  assert.equal(calls.length, 3);
});

test('Ark Assets works without OSS configuration', async () => {
  const assetEnv = {
    MAICHAT_MEDIA_SERVICE_TOKEN: 'abcdef0123456789abcdef0123456789',
    VOLC_ACCESS_KEY_ID: 'ark-ak', VOLC_SECRET_ACCESS_KEY: 'ark-sk'
  };
  const handle = createHandler({ env: assetEnv, callArk: async (action, payload) => {
    assert.equal(action, 'CreateAsset');
    assert.equal(payload.URL, 'https://images.example.com/portrait.png');
    return { Result: { Id: 'asset-1234567890' } };
  } });
  const create = { rawPath: '/ark-assets', requestContext: { http: { method: 'POST' } },
    headers: { Authorization: `Bearer ${assetEnv.MAICHAT_MEDIA_SERVICE_TOKEN}` },
    body: JSON.stringify({ action: 'create_asset', group_id: 'group-1234567890',
      url: 'https://images.example.com/portrait.png' }) };
  assert.equal((await handle(create)).statusCode, 200);
  assert.equal((await handle({ ...create, rawPath: '/sign-upload' })).statusCode, 503);
  const status = await handle({ ...create, rawPath: '/credentials',
    body: JSON.stringify({ action: 'status' }) });
  assert.deepEqual(JSON.parse(status.body), {
    configured: [], assets_configured: true, storage_configured: false
  });
  const fetched = await handle({ ...create, rawPath: '/credentials',
    body: JSON.stringify({ action: 'fetch', providers: ['ark'] }) });
  assert.deepEqual(JSON.parse(fetched.body).api_keys, {});
});

test('synchronizes only requested model keys and never returns AccessKeys', async () => {
  const configured = { ...env,
    MAICHAT_ARK_API_KEY: 'ark-project-key', MAICHAT_GLM_API_KEY: 'glm-key',
    MAICHAT_DEEPSEEK_API_KEY: 'deepseek-key', MAICHAT_WAN_API_KEY: 'wan-key',
    MAICHAT_WAN_WORKSPACE_ID: 'ws-example', MAICHAT_KLING_API_KEY: 'kling-key',
    MAICHAT_MINIMAX_API_KEY: 'minimax-key',
    VOLC_ACCESS_KEY_ID: 'volc-ak', VOLC_SECRET_ACCESS_KEY: 'volc-sk' };
  const handle = createHandler({ env: configured });
  const credentials = body => ({ ...request(body), rawPath: '/credentials' });
  const status = await handle(credentials({ action: 'status' }));
  assert.deepEqual(JSON.parse(status.body).configured,
    ['ark', 'glm', 'deepseek', 'wan', 'kling', 'minimax']);
  const fetched = await handle(credentials({ action: 'fetch',
    providers: ['ark', 'glm', 'glm_video', 'wan', 'kling'] }));
  assert.deepEqual(JSON.parse(fetched.body), {
    api_keys: { ark: 'ark-project-key', glm: 'glm-key', glm_video: 'glm-key',
      wan: 'wan-key', kling: 'kling-key' },
    wan_workspace_id: 'ws-example'
  });
  assert.equal(fetched.headers['Cache-Control'], 'no-store');
  assert.equal(fetched.headers.Pragma, 'no-cache');
  assert.ok(!fetched.body.includes('volc-sk'));
  assert.ok(!fetched.body.includes('deepseek-key'));
  assert.equal((await handle(credentials({ action: 'fetch',
    providers: ['volc_access'] }))).statusCode, 400);
  assert.equal((await handle(credentials({ action: 'fetch',
    providers: ['ark', 'ark'] }))).statusCode, 400);
  assert.equal((await handle({ ...credentials({ action: 'fetch', providers: ['ark'] }),
    headers: { Authorization: 'Bearer wrong' } })).statusCode, 401);
});
