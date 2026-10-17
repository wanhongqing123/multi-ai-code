'use strict';

// 账号服务只从本机模型凭据服务取得短期 Key，不转发模型请求或媒体文件。
function createBailianCredentialIssuer(env, fetcher = fetch) {
  const token = env.MAICHAT_AUTH_SERVICE_TOKEN || '';
  const port = Number(env.MAICHAT_MODEL_CREDENTIAL_PORT || 9200);
  return async function issueBailianCredential() {
    if (token.length < 32 || !Number.isInteger(port) || port < 1 || port > 65535)
      throw new Error('Bailian credential issuer is not configured');
    const response = await fetcher(`http://127.0.0.1:${port}/temporary-credentials`, {
      method: 'POST', redirect: 'error',
      headers: { Authorization: `Bearer ${token}`, 'Content-Type': 'application/json' },
      body: JSON.stringify({ action: 'bailian_video' }),
      signal: AbortSignal.timeout(8000)
    });
    if (!response.ok) throw new Error('Bailian credential issuer rejected the request');
    const raw = await response.text();
    if (Buffer.byteLength(raw) > 2048) throw new Error('Bailian credential response is too large');
    const result = JSON.parse(raw);
    const now = Math.floor(Date.now() / 1000);
    if (typeof result.api_key !== 'string' ||
        !/^st-[A-Za-z0-9._~+/-]{8,4093}={0,2}$/.test(result.api_key) ||
        !Number.isSafeInteger(result.expires_at) || result.expires_at <= now ||
        result.expires_at > now + 360 ||
        !/^(?:ws|llm)-[A-Za-z0-9_-]{1,120}$/.test(result.wan_workspace_id))
      throw new Error('Bailian credential issuer returned an invalid key');
    return result;
  };
}

module.exports = { createBailianCredentialIssuer };
