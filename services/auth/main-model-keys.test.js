// 文件作用：验证账号服务只经带服务令牌的本机回环读取主聊天 Key。
// 测试使用假 Key，不调用供应商或产生付费模型请求。
'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const { createMainModelKeyIssuer } = require('./main-model-keys');

test('account service reads only main chat keys over authenticated loopback', async () => {
  const token = 'abcdef0123456789abcdef0123456789';
  let requests = 0;
  const issue = createMainModelKeyIssuer({ MAICHAT_AUTH_SERVICE_TOKEN: token },
    async (url, options) => {
      ++requests;
      assert.equal(url, 'http://127.0.0.1:9000/account-model-keys');
      assert.equal(options.headers.Authorization, `Bearer ${token}`);
      assert.deepEqual(JSON.parse(options.body), { action: 'main_chat' });
      return { ok: true, async text() { return JSON.stringify({
        api_keys: { glm: 'glm-model-key', deepseek: 'deepseek-model-key' }
      }); } };
    });
  assert.deepEqual(await issue(), {
    api_keys: { glm: 'glm-model-key', deepseek: 'deepseek-model-key' }
  });
  assert.equal(requests, 1);
  await assert.rejects(createMainModelKeyIssuer({})());
});
