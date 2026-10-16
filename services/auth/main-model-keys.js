// 文件作用：账号服务从本机素材服务领取主聊天模型 Key，客户端不接触服务间令牌。
// 此接口只传少量凭据 JSON，不承载模型请求、流式输出或媒体文件。
'use strict';

function createMainModelKeyIssuer(env, fetcher = fetch) {
  const token = env.MAICHAT_AUTH_SERVICE_TOKEN || '';
  const port = Number(env.MAICHAT_MEDIA_PORT || 9000);
  return async function issueMainModelKeys() {
    if (token.length < 32 || !Number.isInteger(port) || port < 1 || port > 65535)
      throw new Error('Main model key issuer is not configured');
    const response = await fetcher(`http://127.0.0.1:${port}/account-model-keys`, {
      method: 'POST', redirect: 'error',
      headers: { Authorization: `Bearer ${token}`, 'Content-Type': 'application/json' },
      body: JSON.stringify({ action: 'main_chat' }),
      signal: AbortSignal.timeout(8000)
    });
    if (!response.ok) throw new Error('Main model key issuer rejected the request');
    const raw = await response.text();
    if (Buffer.byteLength(raw) > 16_384)
      throw new Error('Main model key response is too large');
    const keys = JSON.parse(raw).api_keys;
    if (!keys || typeof keys !== 'object' || Array.isArray(keys) ||
        !['glm', 'deepseek'].every(name => typeof keys[name] === 'string' &&
          keys[name].length <= 4096 && !/[\x00-\x1f\x7f]/.test(keys[name])) ||
        (!keys.glm && !keys.deepseek))
      throw new Error('Main model key response is invalid');
    return { api_keys: { glm: keys.glm, deepseek: keys.deepseek } };
  };
}

module.exports = { createMainModelKeyIssuer };
