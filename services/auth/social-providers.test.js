'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const { createSocialVerifiers } = require('./social-providers');

test('social providers stay unavailable without server-side configuration', () => {
  const verifiers = createSocialVerifiers({});
  assert.equal(verifiers.wechat, undefined);
  assert.equal(verifiers.qq, undefined);
  assert.equal(createSocialVerifiers({ MAICHAT_WECHAT_APP_ID: 'wx-test' }).wechat,
    undefined);
});

test('WeChat code is exchanged by the server and only official identity is used', async () => {
  let calls = 0;
  const verifiers = createSocialVerifiers({
    MAICHAT_WECHAT_APP_ID: 'wx-test',
    MAICHAT_WECHAT_APP_SECRET: 'server-only-test-secret'
  }, async (url, options) => {
    ++calls;
    assert.equal(url.origin, 'https://api.weixin.qq.com');
    assert.equal(url.pathname, '/sns/oauth2/access_token');
    assert.equal(url.searchParams.get('appid'), 'wx-test');
    assert.equal(url.searchParams.get('secret'), 'server-only-test-secret');
    assert.equal(url.searchParams.get('code'), 'one-time-code');
    assert.equal(url.searchParams.get('grant_type'), 'authorization_code');
    assert.equal(options.redirect, 'error');
    return new Response(JSON.stringify({ access_token: 'platform-token',
      openid: 'verified-openid' }));
  });
  assert.deepEqual(await verifiers.wechat('one-time-code'), {
    appId: 'wx-test', subject: 'verified-openid'
  });
  assert.equal(calls, 1);
});

test('QQ token is accepted only when official OpenID belongs to configured AppID', async () => {
  let clientId = 'wrong-app';
  const verifiers = createSocialVerifiers({ MAICHAT_QQ_APP_ID: '100001' },
    async (url, options) => {
      assert.equal(url.origin, 'https://graph.qq.com');
      assert.equal(url.pathname, '/oauth2.0/me');
      assert.equal(url.searchParams.get('access_token'), 'sdk-token');
      assert.equal(url.searchParams.get('fmt'), 'json');
      assert.equal(options.redirect, 'error');
      return new Response(JSON.stringify({ client_id: clientId,
        openid: 'verified-qq-openid' }));
    });
  assert.equal(await verifiers.qq('sdk-token'), null);
  clientId = '100001';
  assert.deepEqual(await verifiers.qq('sdk-token'), {
    appId: '100001', subject: 'verified-qq-openid'
  });
});

test('official credential errors fail closed', async () => {
  const wechat = createSocialVerifiers({
    MAICHAT_WECHAT_APP_ID: 'wx-test', MAICHAT_WECHAT_APP_SECRET: 'server-secret'
  }, async () => new Response(JSON.stringify({ errcode: 40029,
    errmsg: 'invalid code' })));
  assert.equal(await wechat.wechat('bad-code'), null);
  const qq = createSocialVerifiers({ MAICHAT_QQ_APP_ID: '100001' },
    async () => new Response(JSON.stringify({ error: 100016,
      error_description: 'invalid token' })));
  assert.equal(await qq.qq('bad-token'), null);
  const oversized = createSocialVerifiers({ MAICHAT_QQ_APP_ID: '100001' },
    async () => new Response('x'.repeat(16_385)));
  await assert.rejects(oversized.qq('sdk-token'), /too large/);
});
