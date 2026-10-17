// 文件作用：账号服务从本机模型凭据提供者领取账号可用的供应商 Key。
// 此接口只传少量凭据 JSON，不承载模型请求、流式输出或媒体文件。
'use strict';

function createAccountModelKeyIssuer(env, fetcher = fetch) {
  const token = env.MAICHAT_AUTH_SERVICE_TOKEN || '';
  const port = Number(env.MAICHAT_MODEL_CREDENTIAL_PORT || 9200);
  return async function issueAccountModelKeys() {
    if (token.length < 32 || !Number.isInteger(port) || port < 1 || port > 65535)
      throw new Error('Account model key issuer is not configured');
    const response = await fetcher(`http://127.0.0.1:${port}/account-model-keys`, {
      method: 'POST', redirect: 'error',
      headers: { Authorization: `Bearer ${token}`, 'Content-Type': 'application/json' },
      body: JSON.stringify({ action: 'account_models' }),
      signal: AbortSignal.timeout(8000)
    });
    if (!response.ok) throw new Error('Account model key issuer rejected the request');
    const raw = await response.text();
    if (Buffer.byteLength(raw) > 16_384)
      throw new Error('Account model key response is too large');
    const keys = JSON.parse(raw).api_keys;
    if (!keys || typeof keys !== 'object' || Array.isArray(keys) ||
        !['glm', 'deepseek', 'minimax'].every(name => typeof keys[name] === 'string' &&
          keys[name].length <= 4096 && !/[\x00-\x1f\x7f]/.test(keys[name])) ||
        (!keys.glm && !keys.deepseek && !keys.minimax))
      throw new Error('Account model key response is invalid');
    return { api_keys: { glm: keys.glm, deepseek: keys.deepseek,
      minimax: keys.minimax } };
  };
}

module.exports = { createAccountModelKeyIssuer };
