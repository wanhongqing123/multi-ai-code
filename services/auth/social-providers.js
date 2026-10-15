'use strict';

async function readProviderJson(response) {
  if (!response.ok) throw new Error('Social provider request failed');
  const chunks = [];
  let bytes = 0;
  for await (const chunk of response.body) {
    bytes += chunk.byteLength;
    if (bytes > 16_384) throw new Error('Social provider response is too large');
    chunks.push(Buffer.from(chunk));
  }
  const parsed = JSON.parse(Buffer.concat(chunks).toString('utf8'));
  if (!parsed || typeof parsed !== 'object' || Array.isArray(parsed))
    throw new Error('Invalid social provider response');
  return parsed;
}

function createSocialVerifiers(env, request = fetch) {
  const verifiers = {};
  const wechatAppId = env.MAICHAT_WECHAT_APP_ID;
  const wechatSecret = env.MAICHAT_WECHAT_APP_SECRET;
  if (wechatAppId && wechatSecret) {
    verifiers.wechat = async code => {
      const url = new URL('https://api.weixin.qq.com/sns/oauth2/access_token');
      url.searchParams.set('appid', wechatAppId);
      url.searchParams.set('secret', wechatSecret);
      url.searchParams.set('code', code);
      url.searchParams.set('grant_type', 'authorization_code');
      const result = await readProviderJson(await request(url, {
        method: 'GET', redirect: 'error', signal: AbortSignal.timeout(5000)
      }));
      if (result.errcode !== undefined && result.errcode !== 0) return null;
      if (typeof result.openid !== 'string' || !result.openid ||
          typeof result.access_token !== 'string' || !result.access_token)
        throw new Error('Invalid WeChat response');
      return { appId: wechatAppId, subject: result.openid };
    };
  }

  const qqAppId = env.MAICHAT_QQ_APP_ID;
  if (qqAppId) {
    verifiers.qq = async accessToken => {
      const url = new URL('https://graph.qq.com/oauth2.0/me');
      url.searchParams.set('access_token', accessToken);
      url.searchParams.set('fmt', 'json');
      const result = await readProviderJson(await request(url, {
        method: 'GET', redirect: 'error', signal: AbortSignal.timeout(5000)
      }));
      if (result.error !== undefined || result.client_id === undefined) return null;
      if (String(result.client_id) !== String(qqAppId) ||
          typeof result.openid !== 'string' || !result.openid)
        return null;
      return { appId: qqAppId, subject: result.openid };
    };
  }
  return verifiers;
}

module.exports = { createSocialVerifiers };
