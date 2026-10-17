// 文件作用：按账号服务的内部授权签发模型 API Key 与短期供应商凭据。
// 本进程仅监听本机，不接收模型生成请求，也不处理素材文件。
'use strict';

const { timingSafeEqual } = require('node:crypto');
const { callArk: defaultCallArk } = require('../shared/ark-sign');

function jsonResponse(statusCode, value) {
  return { statusCode, headers: { 'Content-Type': 'application/json; charset=utf-8',
    'Cache-Control': 'no-store', Pragma: 'no-cache' }, body: JSON.stringify(value) };
}

function authorized(headers, expected) {
  const value = Object.entries(headers || {}).find(([name]) =>
    name.toLowerCase() === 'authorization')?.[1];
  const actual = Buffer.from(typeof value === 'string' ? value : '');
  const wanted = Buffer.from(`Bearer ${expected}`);
  return actual.length === wanted.length && timingSafeEqual(actual, wanted);
}

function requestBody(event) {
  let body = event.body || '';
  if (event.isBase64Encoded) body = Buffer.from(body, 'base64').toString('utf8');
  if (typeof body !== 'string' || Buffer.byteLength(body) > 4096) return null;
  try { return JSON.parse(body); } catch { return null; }
}

function createHandler(options = {}) {
  const env = options.env || process.env;
  const currentTime = options.currentTime || (() => new Date());
  const fetcher = options.fetcher || fetch;
  const callArk = options.callArk || defaultCallArk;
  return async function handle(eventInput) {
    let event;
    try {
      event = typeof eventInput === 'string' || Buffer.isBuffer(eventInput)
        ? JSON.parse(eventInput.toString()) : eventInput;
    } catch { return jsonResponse(400, { error: 'Invalid request' }); }
    const method = event?.requestContext?.http?.method || event?.httpMethod || event?.method;
    const route = event?.rawPath || event?.path;
    if (method !== 'POST' ||
        (route !== '/account-model-keys' && route !== '/temporary-credentials'))
      return jsonResponse(404, { error: 'Not found' });
    const token = env.MAICHAT_AUTH_SERVICE_TOKEN || '';
    if (token.length < 32) return jsonResponse(503, { error: 'Service token is not configured' });
    if (!authorized(event.headers, token)) return jsonResponse(401, { error: 'Unauthorized' });
    const body = requestBody(event);
    if (route === '/account-model-keys') {
      if (body?.action !== 'account_models')
        return jsonResponse(400, { error: 'Invalid model key request' });
      const glm = env.MAICHAT_GLM_API_KEY || '';
      const deepseek = env.MAICHAT_DEEPSEEK_API_KEY || '';
      const minimax = env.MAICHAT_MINIMAX_API_KEY || '';
      if (!glm && !deepseek && !minimax)
        return jsonResponse(503, { error: 'Account models are not configured' });
      return jsonResponse(200, { api_keys: { glm, deepseek, minimax } });
    }
    if (route === '/temporary-credentials') {
      if (body?.action === 'bailian_video') {
        const permanentKey = env.MAICHAT_WAN_API_KEY || '';
        const workspace = env.MAICHAT_WAN_WORKSPACE_ID || '';
        if (!permanentKey || !/^(?:ws|llm)-[A-Za-z0-9_-]{1,120}$/.test(workspace))
          return jsonResponse(503, { error: 'Bailian temporary credentials are not configured' });
        // 百炼临时 Key 继承主 Key 的权限，主 Key 始终只留在服务端。
        let response;
        try {
          response = await fetcher(
            'https://dashscope.aliyuncs.com/api/v1/tokens?expire_in_seconds=300', {
              method: 'POST', redirect: 'error',
              headers: { Authorization: `Bearer ${permanentKey}` },
              signal: AbortSignal.timeout(8000)
            });
        } catch {
          return jsonResponse(502, { error: 'Bailian credential issuer is unavailable' });
        }
        if (!response.ok)
          return jsonResponse(502, { error: 'Bailian credential issuer rejected the request' });
        let issued;
        try {
          const raw = await response.text();
          if (Buffer.byteLength(raw) > 2048) throw new Error('Oversized response');
          issued = JSON.parse(raw);
        } catch {
          return jsonResponse(502, { error: 'Bailian credential response is invalid' });
        }
        const nowSeconds = Math.floor(currentTime().getTime() / 1000);
        if (typeof issued.token !== 'string' ||
            !/^st-[A-Za-z0-9._~+/-]{8,4093}={0,2}$/.test(issued.token) ||
            !Number.isSafeInteger(issued.expires_at) ||
            issued.expires_at <= nowSeconds || issued.expires_at > nowSeconds + 360)
          return jsonResponse(502, { error: 'Bailian returned an invalid temporary credential' });
        return jsonResponse(200, { api_key: issued.token, expires_at: issued.expires_at,
          wan_workspace_id: workspace });
      }
      if (body?.action !== 'ark_seedance')
        return jsonResponse(400, { error: 'Invalid temporary credential request' });
      const models = (env.MAICHAT_ARK_MODEL_IDS || '').split(',').map(v => v.trim())
        .filter(Boolean);
      if (!models.length || models.length > 10 ||
          !models.every(v => /^doubao-(?:seedance|seedream)-[a-z0-9-]{1,70}$/.test(v)) ||
          new Set(models).size !== models.length ||
          !env.VOLC_ACCESS_KEY_ID || !env.VOLC_SECRET_ACCESS_KEY)
        return jsonResponse(503, { error: 'Ark media temporary credentials are not configured' });
      const issued = await callArk('GetApiKey', {
        DurationSeconds: 300, ResourceType: 'presetendpoint', ResourceIds: models,
        ProjectName: env.VOLC_ARK_PROJECT_NAME || 'default'
      }, { accessKeyId: env.VOLC_ACCESS_KEY_ID,
           secretKey: env.VOLC_SECRET_ACCESS_KEY });
      const result = issued?.Result || {};
      const nowSeconds = Math.floor(currentTime().getTime() / 1000);
      if (typeof result.ApiKey !== 'string' || result.ApiKey.length < 16 ||
          !Number.isSafeInteger(result.ExpiredTime) ||
          result.ExpiredTime <= nowSeconds || result.ExpiredTime > nowSeconds + 360)
        return jsonResponse(502, { error: 'Ark returned an invalid temporary credential' });
      return jsonResponse(200, { api_key: result.ApiKey,
        expires_at: result.ExpiredTime });
    }
    return jsonResponse(404, { error: 'Not found' });
  };
}

const handler = createHandler();
module.exports = { createHandler, handler };
