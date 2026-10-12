'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const { createArkCredentialIssuer } = require('./ark-credential');

test('account service fetches only a short-lived key from loopback media service', async () => {
  const token = 'abcdef0123456789abcdef0123456789';
  const issued = Math.floor(Date.now() / 1000) + 300;
  const requests = [];
  const issue = createArkCredentialIssuer({ MAICHAT_AUTH_SERVICE_TOKEN: token },
    async (url, options) => {
      requests.push({ url, options });
      return { ok: true, async text() { return JSON.stringify({
        api_key: 'temporary-key-for-seedance', expires_at: issued
      }); } };
    });
  assert.deepEqual(await issue(), {
    api_key: 'temporary-key-for-seedance', expires_at: issued
  });
  assert.equal(requests[0].url, 'http://127.0.0.1:9000/temporary-credentials');
  assert.equal(requests[0].options.headers.Authorization, `Bearer ${token}`);
  assert.deepEqual(JSON.parse(requests[0].options.body), { action: 'ark_seedance' });
  await assert.rejects(createArkCredentialIssuer({})(/* no credentials */));
});
