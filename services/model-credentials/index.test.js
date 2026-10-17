// 文件作用：验证模型凭据进程仅向持有内部授权的账号服务签发模型 Key。
// 测试全部使用假供应商响应，不提交付费模型任务。
'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const { createHandler } = require('./index');

const env = {};
const accountToken = 'a'.repeat(43);
function request(body, token = accountToken) {
  return { rawPath: '/temporary-credentials', httpMethod: 'POST',
    headers: { Authorization: `Bearer ${token}` }, body: JSON.stringify(body) };
}

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
  assert.equal((await handle(issue(accountToken))).statusCode, 401);
  assert.equal(calls, 0);
  const result = await handle(issue(internalToken));
  assert.equal(result.statusCode, 200);
  assert.deepEqual(JSON.parse(result.body), {
    api_key: 'st-temporary-bailian-key',
    expires_at: Math.floor(now.getTime() / 1000) + 300,
    wan_workspace_id: 'ws-test'
  });
  assert.ok(!result.body.includes('permanent-bailian-key'));
  const removed = await handle({ ...request({ action: 'fetch',
    providers: ['wan', 'kling'] }), rawPath: '/credentials' });
  assert.equal(removed.statusCode, 404);
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

test('account-only main model key route never accepts the client service token', async () => {
  const authToken = 'fedcba9876543210fedcba9876543210';
  const configured = { ...env, MAICHAT_AUTH_SERVICE_TOKEN: authToken,
    MAICHAT_GLM_API_KEY: 'glm-model-key', MAICHAT_DEEPSEEK_API_KEY: 'deepseek-model-key',
    MAICHAT_MINIMAX_API_KEY: 'minimax-model-key' };
  const handle = createHandler({ env: configured });
  const requestKeys = token => ({ ...request({ action: 'account_models' }, token),
    rawPath: '/account-model-keys' });
  assert.equal((await handle(requestKeys(accountToken))).statusCode, 401);
  const issued = await handle(requestKeys(authToken));
  assert.equal(issued.statusCode, 200);
  assert.deepEqual(JSON.parse(issued.body), {
    api_keys: { glm: 'glm-model-key', deepseek: 'deepseek-model-key',
      minimax: 'minimax-model-key' }
  });
  assert.ok(!issued.body.includes('fedcba9876543210'));
  assert.equal((await handle({ ...requestKeys(authToken),
    body: JSON.stringify({ action: 'fetch' }) })).statusCode, 400);
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
  assert.equal((await handle(issue(accountToken))).statusCode, 401);
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
