'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const { createTencentAccountDeleter } = require('./tim-account-delete');

test('deletion checks both Tencent top-level and per-account results', async () => {
  let itemCode = 0;
  const calls = [];
  const remove = createTencentAccountDeleter({ sdkAppId: 123456789,
    imSecret: 'test-only-im-secret-keep-on-server-123456',
    fetchImpl: async (url, options) => {
      calls.push({ url, options });
      return { ok: true, text: async () => JSON.stringify({ ActionStatus: 'OK',
        ErrorCode: 0, ResultItem: [{ UserID: 'im_test_user', ResultCode: itemCode }] }) };
    } });
  assert.equal((await remove('im_test_user')).result_code, 0);
  assert.equal(calls[0].url.pathname, '/v4/im_open_login_svc/account_delete');
  assert.deepEqual(JSON.parse(calls[0].options.body),
    { DeleteItem: [{ UserID: 'im_test_user' }] });
  itemCode = 71000;
  await assert.rejects(remove('im_test_user'), /71000/);
  itemCode = 70107;
  assert.equal((await remove('im_test_user')).result_code, 70107);
  await assert.rejects(remove('bad user'), /Invalid IM account ID/);
});
