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

test('signs a private Ark video PUT and temporary GET for a random object key', async () => {
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
  const result = await handle(request({ filename: 'clip.MOV', size_bytes: 20_000_000,
    purpose: 'ark_asset_input' }));
  assert.equal(result.statusCode, 200);
  const body = JSON.parse(result.body);
  assert.equal(body.object_key, 'ark-asset-inputs/2026-10-08/1234-unique.mov');
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
  assert.equal((await handle(request({ filename: 'clip.mp4', size_bytes: 1,
    purpose: 'ark_asset_input' }, 'wrong'))).statusCode, 401);
  assert.equal((await handle(request({ filename: 'clip.mp4', size_bytes: 200_000_001,
    purpose: 'ark_asset_input' }))).statusCode, 400);
  assert.equal((await handle(request({ filename: 'clip.exe', size_bytes: 12,
    purpose: 'ark_asset_input' }))).statusCode, 400);
  assert.equal((await handle(request({ filename: 'photo.jpg', size_bytes: 30_000_001,
    purpose: 'ark_asset_input' }))).statusCode, 400);
  assert.equal(created, false);
});

test('mints a bounded Bailian key only for the account service', async () => {
  const now = new Date('2026-10-08T00:00:00Z');
  const internalToken = 'fedcba9876543210fedcba9876543210';
  const configured = { ...env, MAICHAT_AUTH_SERVICE_TOKEN: internalToken,
    MAICHAT_WAN_API_KEY: 'permanent-bailian-key', MAICHAT_WAN_WORKSPACE_ID: 'ws-test' };
  let calls = 0;
  const handle = createHandler({ env: configured, currentTime: () => now,
    fetcher: async (url, options) => {
      ++calls;
      assert.equal(url, 'https://dashscope.aliyuncs.com/api/v1/tokens?expire_in_seconds=300');
      assert.equal(options.method, 'POST');
      assert.equal(options.headers.Authorization, 'Bearer permanent-bailian-key');
      return { ok: true, async text() { return JSON.stringify({
        token: 'st-temporary-bailian-key',
        expires_at: Math.floor(now.getTime() / 1000) + 300
      }); } };
    } });
  const issue = token => ({ ...request({ action: 'bailian_video' }, token),
    rawPath: '/temporary-credentials' });
  assert.equal((await handle(issue(env.MAICHAT_OSS_SIGNER_TOKEN))).statusCode, 401);
  assert.equal(calls, 0);
  const result = await handle(issue(internalToken));
  assert.equal(result.statusCode, 200);
  assert.deepEqual(JSON.parse(result.body), {
    api_key: 'st-temporary-bailian-key',
    expires_at: Math.floor(now.getTime() / 1000) + 300,
    wan_workspace_id: 'ws-test'
  });
  assert.ok(!result.body.includes('permanent-bailian-key'));
  const legacy = await handle({ ...request({ action: 'fetch',
    providers: ['wan', 'kling'] }), rawPath: '/credentials' });
  assert.deepEqual(JSON.parse(legacy.body).api_keys, {});
  assert.equal(calls, 1);
});

test('does not forward an expired or malformed Bailian response', async () => {
  const now = new Date('2026-10-08T00:00:00Z');
  const configured = { ...env,
    MAICHAT_AUTH_SERVICE_TOKEN: 'fedcba9876543210fedcba9876543210',
    MAICHAT_WAN_API_KEY: 'permanent-bailian-key', MAICHAT_WAN_WORKSPACE_ID: 'ws-test' };
  const body = { action: 'bailian_video' };
  const requestFor = () => ({ ...request(body, configured.MAICHAT_AUTH_SERVICE_TOKEN),
    rawPath: '/temporary-credentials' });
  for (const issued of [
    { token: 'st-temporary-bailian-key', expires_at: Math.floor(now.getTime() / 1000) - 1 },
    { token: 'permanent-bailian-key', expires_at: Math.floor(now.getTime() / 1000) + 300 }
  ]) {
    const handle = createHandler({ env: configured, currentTime: () => now,
      fetcher: async () => ({ ok: true, async text() { return JSON.stringify(issued); } }) });
    assert.equal((await handle(requestFor())).statusCode, 502);
  }
});

test('signs direct JPEG uploads for private Ark input storage', async () => {
  const calls = [];
  const handle = createHandler({ env, newID: () => 'image-id',
    currentTime: () => new Date('2026-10-08T00:00:00Z'),
    makeClient: () => ({ signatureUrlV4: async (...args) => {
      calls.push(args);
      return `https://example.oss-cn-hangzhou.aliyuncs.com/${args[3]}?signed=yes`;
    } }) });
  const result = await handle(request({ filename: 'photo.JPG', size_bytes: 1_000_000,
    purpose: 'ark_asset_input' }));
  assert.equal(result.statusCode, 200);
  const body = JSON.parse(result.body);
  assert.equal(body.object_key, 'ark-asset-inputs/2026-10-08/image-id.jpg');
  assert.equal(body.upload_headers['Content-Type'], 'image/jpeg');
  assert.deepEqual(calls.map(call => call[0]), ['PUT', 'GET']);
});

test('routes explicit upload purposes to fixed prefixes for supported file types', async () => {
  const signedKeys = [];
  const handle = createHandler({ env, newID: () => 'fixed-id',
    currentTime: () => new Date('2026-10-08T00:00:00Z'),
    makeClient: () => ({ signatureUrlV4: async (...args) => {
      signedKeys.push(args[3]);
      return `https://example.oss-cn-hangzhou.aliyuncs.com/${args[3]}?signed=yes`;
    } }) });
  const cases = [
    ['wan_input', 'photo.JPG', 'wan-inputs/2026-10-08/fixed-id.jpg'],
    ['wan_input', 'photo.PNG', 'wan-inputs/2026-10-08/fixed-id.png'],
    ['wan_input', 'clip.MP4', 'wan-inputs/2026-10-08/fixed-id.mp4'],
    ['wan_input', 'clip.MOV', 'wan-inputs/2026-10-08/fixed-id.mov'],
    ['wan_input', 'proposal.PDF', 'wan-inputs/2026-10-08/fixed-id.pdf'],
    ['ark_asset_input', 'photo.png', 'ark-asset-inputs/2026-10-08/fixed-id.png'],
    ['ark_asset_input', 'clip.mp4', 'ark-asset-inputs/2026-10-08/fixed-id.mp4'],
    ['ark_asset_input', 'clip.mov', 'ark-asset-inputs/2026-10-08/fixed-id.mov'],
    ['shared_media_input', 'photo.png', 'shared-media-inputs/2026-10-08/fixed-id.png'],
    ['shared_media_input', 'clip.mov', 'shared-media-inputs/2026-10-08/fixed-id.mov'],
    ['shared_media_input', 'proposal.pdf', 'shared-media-inputs/2026-10-08/fixed-id.pdf']
  ];
  for (const [purpose, filename, expectedKey] of cases) {
    const result = await handle(request({ filename, size_bytes: 100, purpose }));
    assert.equal(result.statusCode, 200);
    assert.equal(JSON.parse(result.body).object_key, expectedKey);
  }
  assert.equal(signedKeys.length, cases.length * 2);
});

test('rejects unknown or mismatched upload purposes before signing', async () => {
  let created = false;
  const handle = createHandler({ env, makeClient: () => { created = true; } });
  for (const body of [
    { filename: 'photo.jpg', size_bytes: 100 },
    { filename: 'photo.jpg', size_bytes: 100, purpose: 'wan-inputs/../seedance-inputs' },
    { filename: 'photo.jpg', size_bytes: 100, purpose: '__proto__' },
    { filename: 'photo.jpg', size_bytes: 100, purpose: '' },
    { filename: 'photo.jpg', size_bytes: 100, purpose: null },
    { filename: 'photo.jpg', size_bytes: 100, purpose: 'seedance_input' },
    { filename: 'clip.mp4', size_bytes: 100, purpose: 'seedance_input' },
    { filename: 'proposal.pdf', size_bytes: 100, purpose: 'ark_asset_input' },
    { filename: 'proposal.pdf', size_bytes: 100, purpose: 'seedance_input' }
  ]) {
    assert.equal((await handle(request(body))).statusCode, 400);
  }
  assert.equal(created, false);
});

test('never accepts a caller-supplied object path', async () => {
  const handle = createHandler({ env, newID: () => 'safe-id',
    currentTime: () => new Date('2026-10-08T00:00:00Z'),
    makeClient: () => ({ signatureUrlV4: async (...args) =>
      `https://example.oss-cn-hangzhou.aliyuncs.com/${args[3]}?signed=yes` }) });
  const result = await handle(request({ filename: '../../private/photo.jpg',
    size_bytes: 100, purpose: 'wan_input', prefix: 'another-prefix',
    object_key: '../../private/overwrite.jpg' }));
  assert.equal(result.statusCode, 200);
  assert.equal(JSON.parse(result.body).object_key,
    'wan-inputs/2026-10-08/safe-id.jpg');
});

test('signs private Wan document uploads with their real MIME type and 100 MB cap', async () => {
  const calls = [];
  const handle = createHandler({ env, newID: () => 'document-id',
    currentTime: () => new Date('2026-10-08T00:00:00Z'),
    makeClient: () => ({ signatureUrlV4: async (...args) => {
      calls.push(args);
      return `https://example.oss-cn-hangzhou.aliyuncs.com/${args[3]}?signed=yes`;
    } }) });
  const result = await handle(request({ filename: 'proposal.PPTX', size_bytes: 2_000_000,
    purpose: 'wan_input' }));
  assert.equal(result.statusCode, 200);
  const signed = JSON.parse(result.body);
  assert.equal(signed.object_key, 'wan-inputs/2026-10-08/document-id.pptx');
  assert.equal(signed.upload_headers['Content-Type'],
    'application/vnd.openxmlformats-officedocument.presentationml.presentation');
  assert.deepEqual(calls.map(call => call[0]), ['PUT', 'GET']);
  assert.equal((await handle(request({ filename: 'proposal.pptx',
    size_bytes: 100_000_001, purpose: 'wan_input' }))).statusCode, 400);
});

test('fails closed when credentials or route are missing', async () => {
  const missing = createHandler({ env: { ...env, ALIBABA_CLOUD_ACCESS_KEY_SECRET: '' } });
  assert.equal((await missing(request({ filename: 'clip.mp4', size_bytes: 12,
    purpose: 'ark_asset_input' }))).statusCode, 503);
  const active = createHandler({ env });
  const wrongRoute = request({ filename: 'clip.mp4', size_bytes: 12,
    purpose: 'ark_asset_input' });
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

test('forwards only supported Ark asset types and defaults old clients to Image', async () => {
  const assetEnv = { ...env, VOLC_ACCESS_KEY_ID: 'ark-ak',
    VOLC_SECRET_ACCESS_KEY: 'ark-sk' };
  const calls = [];
  const handle = createHandler({ env: assetEnv, callArk: async (action, payload) => {
    calls.push({ action, payload });
    return { Result: { Id: 'asset-1234567890' } };
  } });
  const assetRequest = body => ({ ...request(body), rawPath: '/ark-assets' });
  const base = { action: 'create_asset', group_id: 'group-1234567890',
    url: 'https://videos.example.com/clip.mp4' };
  assert.equal((await handle(assetRequest({ ...base, asset_type: 'Video' }))).statusCode, 200);
  assert.deepEqual(calls[0], { action: 'CreateAsset', payload: {
    GroupId: 'group-1234567890', URL: base.url, AssetType: 'Video',
    Name: 'Video', ProjectName: 'default'
  } });
  assert.equal((await handle(assetRequest({ ...base, asset_type: 'Image' }))).statusCode, 200);
  assert.equal(calls[1].payload.AssetType, 'Image');
  assert.equal((await handle(assetRequest(base))).statusCode, 200);
  assert.equal(calls[2].payload.AssetType, 'Image');
  for (const assetType of ['Audio', 'video', '', null, '../Video']) {
    assert.equal((await handle(assetRequest({ ...base,
      asset_type: assetType }))).statusCode, 400);
  }
  assert.equal((await handle(assetRequest({ ...base, asset_type: 'Video',
    url: 'http://localhost/clip.mp4' }))).statusCode, 400);
  assert.equal((await handle(assetRequest({ ...base, asset_type: 'Video',
    group_id: '../group-123' }))).statusCode, 400);
  assert.equal(calls.length, 3);
});

test('looks up an Ark group by validated ID without returning service credentials', async () => {
  const assetEnv = { ...env, VOLC_ACCESS_KEY_ID: 'ark-ak',
    VOLC_SECRET_ACCESS_KEY: 'ark-sk', VOLC_ARK_PROJECT_NAME: 'real-project' };
  const calls = [];
  const handle = createHandler({ env: assetEnv, callArk: async (action, payload, credentials) => {
    calls.push({ action, payload, credentials });
    return { Result: { Id: 'group-real-123456', GroupType: 'LivenessFace' } };
  } });
  const assetRequest = body => ({ ...request(body), rawPath: '/ark-assets' });
  const result = await handle(assetRequest({ action: 'get_group',
    group_id: 'group-real-123456' }));
  assert.equal(result.statusCode, 200);
  assert.deepEqual(JSON.parse(result.body), { result: {
    Id: 'group-real-123456', GroupType: 'LivenessFace'
  } });
  assert.equal(calls[0].action, 'GetAssetGroup');
  assert.deepEqual(calls[0].payload, {
    Id: 'group-real-123456', ProjectName: 'real-project'
  });
  assert.equal(calls[0].credentials.secretKey, 'ark-sk');
  assert.ok(!result.body.includes('ark-sk'));
  for (const groupId of ['', 'asset-123456', '../group-real-123456']) {
    assert.equal((await handle(assetRequest({ action: 'get_group',
      group_id: groupId }))).statusCode, 400);
  }
  assert.equal(calls.length, 1);
});

test('checks the expected Ark group type before creating a real-person video asset', async () => {
  const assetEnv = { ...env, VOLC_ACCESS_KEY_ID: 'ark-ak',
    VOLC_SECRET_ACCESS_KEY: 'ark-sk' };
  const calls = [];
  let actualGroupType = 'LivenessFace';
  const handle = createHandler({ env: assetEnv, callArk: async (action, payload) => {
    calls.push({ action, payload });
    if (action === 'GetAssetGroup') return { Result: {
      Id: 'group-real-123456', GroupType: actualGroupType
    } };
    return { Result: { Id: 'asset-video-123456' } };
  } });
  const assetRequest = body => ({ ...request(body), rawPath: '/ark-assets' });
  const video = { action: 'create_asset', group_id: 'group-real-123456',
    url: 'https://videos.example.com/clip.mp4', asset_type: 'Video',
    expected_group_type: 'LivenessFace' };
  const created = await handle(assetRequest(video));
  assert.equal(created.statusCode, 200);
  assert.deepEqual(calls.map(call => call.action), ['GetAssetGroup', 'CreateAsset']);
  assert.deepEqual(calls[0].payload, {
    Id: 'group-real-123456', ProjectName: 'default'
  });
  assert.equal(calls[1].payload.AssetType, 'Video');

  actualGroupType = 'AIGC';
  const mismatch = await handle(assetRequest(video));
  assert.equal(mismatch.statusCode, 409);
  assert.equal(calls.at(-1).action, 'GetAssetGroup');
  assert.equal(calls.filter(call => call.action === 'CreateAsset').length, 1);

  for (const expectedGroupType of ['', 'RealPerson', null, '../LivenessFace']) {
    assert.equal((await handle(assetRequest({ ...video,
      expected_group_type: expectedGroupType }))).statusCode, 400);
  }
  assert.equal(calls.length, 3);

  actualGroupType = '';
  assert.equal((await handle(assetRequest(video))).statusCode, 502);
  assert.equal(calls.filter(call => call.action === 'CreateAsset').length, 1);
  assert.ok(!created.body.includes('ark-sk'));
});

test('does not create an Ark asset when the group lookup fails', async () => {
  const assetEnv = { ...env, VOLC_ACCESS_KEY_ID: 'ark-ak',
    VOLC_SECRET_ACCESS_KEY: 'ark-sk' };
  const actions = [];
  const handle = createHandler({ env: assetEnv, callArk: async action => {
    actions.push(action);
    return { ResponseMetadata: { RequestId: 'request-123', Error: {
      Code: 'GroupUnavailable', Message: 'Group lookup failed'
    } } };
  } });
  const result = await handle({ ...request({ action: 'create_asset',
    group_id: 'group-real-123456', url: 'https://videos.example.com/clip.mp4',
    asset_type: 'Video', expected_group_type: 'LivenessFace' }),
    rawPath: '/ark-assets' });
  assert.equal(result.statusCode, 502);
  assert.equal(JSON.parse(result.body).provider_code, 'GroupUnavailable');
  assert.deepEqual(actions, ['GetAssetGroup']);
  assert.ok(!result.body.includes('ark-sk'));
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

test('reports Ark upstream status, code, and request id without exposing credentials', async () => {
  const assetEnv = { ...env, VOLC_ACCESS_KEY_ID: 'ark-ak',
    VOLC_SECRET_ACCESS_KEY: 'ark-sk' };
  const handle = createHandler({ env: assetEnv, callArk: async () => {
    throw { response: { status: 503, data: { ResponseMetadata: {
      RequestId: 'provider-request-123', Error: {
        Code: 'ServiceUnavailable', Message: 'Try again later' }
    } } } };
  } });
  const result = await handle({ ...request({ action: 'list_groups' }), rawPath: '/ark-assets' });
  assert.equal(result.statusCode, 502);
  const body = JSON.parse(result.body);
  assert.equal(body.provider_http_status, 503);
  assert.equal(body.provider_code, 'ServiceUnavailable');
  assert.equal(body.request_id, 'provider-request-123');
  assert.ok(!result.body.includes('ark-sk'));
});

test('creates a real-portrait H5 link and resolves its verified group', async () => {
  const calls = [];
  const assetEnv = { ...env, VOLC_ACCESS_KEY_ID: 'ark-ak',
    VOLC_SECRET_ACCESS_KEY: 'ark-sk' };
  const handle = createHandler({ env: assetEnv, callArk: async (action, payload) => {
    calls.push({ action, payload });
    if (action === 'CreateVisualValidateSession')
      return { Result: { BytedToken: 'vvs-test-token',
        H5Link: 'https://ark.example.com/verify?token=test' } };
    if (action === 'GetVisualValidateResult')
      return { Result: { GroupId: 'group-real-123456' } };
    return { Result: { Items: [] } };
  } });
  const assetRequest = body => ({ ...request(body), rawPath: '/ark-assets' });
  const begin = await handle(assetRequest({ action: 'begin_real_validation' }));
  assert.equal(begin.statusCode, 200);
  assert.equal(JSON.parse(begin.body).result.BytedToken, 'vvs-test-token');
  assert.deepEqual(calls[0], { action: 'CreateVisualValidateSession', payload: {
    CallbackURL: 'https://ichat.life/maichat/portrait-auth-callback',
    ProjectName: 'default' } });
  const finish = await handle(assetRequest({ action: 'get_real_validation',
    byted_token: 'vvs-test-token' }));
  assert.equal(JSON.parse(finish.body).result.GroupId, 'group-real-123456');
  assert.equal(calls[1].payload.BytedToken, 'vvs-test-token');
  await handle(assetRequest({ action: 'list_groups', group_type: 'LivenessFace' }));
  assert.equal(calls[2].payload.Filter.GroupType, 'LivenessFace');
  const invalid = await handle(assetRequest({ action: 'get_real_validation', byted_token: '' }));
  assert.equal(invalid.statusCode, 400);
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
    ['glm', 'deepseek', 'wan', 'kling', 'minimax']);
  const fetched = await handle(credentials({ action: 'fetch',
    providers: ['ark', 'glm', 'glm_video', 'wan', 'kling'] }));
  assert.deepEqual(JSON.parse(fetched.body), {
    api_keys: { glm: 'glm-key', glm_video: 'glm-key' },
    wan_workspace_id: 'ws-example'
  });
  assert.equal(fetched.headers['Cache-Control'], 'no-store');
  assert.equal(fetched.headers.Pragma, 'no-cache');
  assert.ok(!fetched.body.includes('volc-sk'));
  assert.ok(!fetched.body.includes('ark-project-key'));
  assert.ok(!fetched.body.includes('deepseek-key'));
  assert.equal((await handle(credentials({ action: 'fetch',
    providers: ['volc_access'] }))).statusCode, 400);
  assert.equal((await handle(credentials({ action: 'fetch',
    providers: ['ark', 'ark'] }))).statusCode, 400);
  assert.equal((await handle({ ...credentials({ action: 'fetch', providers: ['ark'] }),
    headers: { Authorization: 'Bearer wrong' } })).statusCode, 401);
});

test('issues a five-minute Seedance and Seedream key only to the account service', async () => {
  const now = new Date('2026-10-08T00:00:00Z');
  const internalToken = 'fedcba9876543210fedcba9876543210';
  const models = ['doubao-seedance-2-0-mini-260615',
    'doubao-seedream-5-0-flash-260915'];
  const configured = { ...env,
    MAICHAT_AUTH_SERVICE_TOKEN: internalToken,
    MAICHAT_ARK_MODEL_IDS: models.join(','),
    VOLC_ACCESS_KEY_ID: 'test-volc-id',
    VOLC_SECRET_ACCESS_KEY: 'test-volc-secret'
  };
  let calls = 0;
  const handle = createHandler({ env: configured, currentTime: () => now,
    callArk: async (action, payload, credentials) => {
      ++calls;
      assert.equal(action, 'GetApiKey');
      assert.deepEqual(payload, { DurationSeconds: 300, ResourceType: 'presetendpoint',
        ResourceIds: models, ProjectName: 'default' });
      assert.equal(credentials.accessKeyId, configured.VOLC_ACCESS_KEY_ID);
      return { Result: { ApiKey: 'temporary-ark-model-key',
        ExpiredTime: Math.floor(now.getTime() / 1000) + 300 } };
    } });
  const issue = token => ({ ...request({ action: 'ark_seedance' }, token),
    rawPath: '/temporary-credentials' });
  assert.equal((await handle(issue(env.MAICHAT_OSS_SIGNER_TOKEN))).statusCode, 401);
  assert.equal(calls, 0);
  const result = await handle(issue(internalToken));
  assert.equal(result.statusCode, 200);
  assert.deepEqual(JSON.parse(result.body), { api_key: 'temporary-ark-model-key',
    expires_at: Math.floor(now.getTime() / 1000) + 300 });
  assert.equal(calls, 1);
  assert.ok(!result.body.includes(configured.VOLC_SECRET_ACCESS_KEY));
  const unrelated = createHandler({ env: { ...configured,
    MAICHAT_ARK_MODEL_IDS: 'doubao-seed-2-0-pro-260215' },
    callArk: async () => { throw new Error('must not issue'); } });
  assert.equal((await unrelated(issue(internalToken))).statusCode, 503);
});
