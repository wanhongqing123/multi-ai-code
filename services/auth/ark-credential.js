'use strict';

// 账号服务只通过本机回环访问模型凭据服务，转发的是一枚短期凭据，不承载模型请求或媒体字节。
function createArkCredentialIssuer(env, fetcher = fetch) {
  const token = env.MAICHAT_AUTH_SERVICE_TOKEN || '';
  const port = Number(env.MAICHAT_MODEL_CREDENTIAL_PORT || 9200);
  return async function issueArkCredential() {
    if (token.length < 32 || !Number.isInteger(port) || port < 1 || port > 65535)
      throw new Error('Ark credential issuer is not configured');
    const response = await fetcher(`http://127.0.0.1:${port}/temporary-credentials`, {
      method: 'POST', redirect: 'error',
      headers: { Authorization: `Bearer ${token}`, 'Content-Type': 'application/json' },
      body: JSON.stringify({ action: 'ark_seedance' }),
      signal: AbortSignal.timeout(8000)
    });
    if (!response.ok) throw new Error('Ark credential issuer rejected the request');
    const raw = await response.text();
    if (Buffer.byteLength(raw) > 2048) throw new Error('Ark credential response is too large');
    const result = JSON.parse(raw);
    const now = Math.floor(Date.now() / 1000);
    if (typeof result.api_key !== 'string' || result.api_key.length < 16 ||
        !Number.isSafeInteger(result.expires_at) || result.expires_at <= now ||
        result.expires_at > now + 360)
      throw new Error('Ark credential issuer returned an invalid key');
    return result;
  };
}

module.exports = { createArkCredentialIssuer };
