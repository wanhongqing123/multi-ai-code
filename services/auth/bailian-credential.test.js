'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const { createBailianCredentialIssuer } = require('./bailian-credential');

test('account service fetches a bounded Bailian key from loopback', async () => {
  const token = 'abcdef0123456789abcdef0123456789';
  const issued = Math.floor(Date.now() / 1000) + 300;
  const requests = [];
  const issue = createBailianCredentialIssuer({ MAICHAT_AUTH_SERVICE_TOKEN: token },
    async (url, options) => {
      requests.push({ url, options });
      return { ok: true, async text() { return JSON.stringify({
        api_key: 'st-temporary-bailian-key', expires_at: issued,
        wan_workspace_id: 'ws-test'
      }); } };
    });
  assert.deepEqual(await issue(), {
    api_key: 'st-temporary-bailian-key', expires_at: issued,
    wan_workspace_id: 'ws-test'
  });
  assert.equal(requests[0].url, 'http://127.0.0.1:9200/temporary-credentials');
  assert.equal(requests[0].options.headers.Authorization, `Bearer ${token}`);
  assert.deepEqual(JSON.parse(requests[0].options.body), { action: 'bailian_video' });
  await assert.rejects(createBailianCredentialIssuer({})());
});
