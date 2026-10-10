'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const { createServer } = require('./server');
const { createHandler } = require('./index');

test('HTTP adapter keeps the service on an authenticated route', async () => {
  const env = {
    MAICHAT_MEDIA_SERVICE_TOKEN: '0123456789abcdef0123456789abcdef',
    VOLC_ACCESS_KEY_ID: 'test-ak', VOLC_SECRET_ACCESS_KEY: 'test-sk'
  };
  const server = createServer(createHandler({ env, callArk: async () => ({
    Result: { Id: 'group-1234567890' }
  }) }));
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  try {
    const address = `http://127.0.0.1:${server.address().port}/ark-assets`;
    const unauthorized = await fetch(address, { method: 'POST', body: '{}',
      headers: { 'Content-Type': 'application/json' } });
    assert.equal(unauthorized.status, 401);
    const authorized = await fetch(address, { method: 'POST',
      headers: { Authorization: `Bearer ${env.MAICHAT_MEDIA_SERVICE_TOKEN}`,
        'Content-Type': 'application/json' },
      body: JSON.stringify({ action: 'create_group', name: 'avatar' }) });
    assert.equal(authorized.status, 200);
    assert.equal((await authorized.json()).result.Id, 'group-1234567890');
    const oversized = await fetch(address, { method: 'POST',
      headers: { Authorization: `Bearer ${env.MAICHAT_MEDIA_SERVICE_TOKEN}` },
      body: 'x'.repeat(5000) });
    assert.equal(oversized.status, 413);
    const callback = await fetch(`http://127.0.0.1:${server.address().port}` +
      '/portrait-auth-callback?bytedToken=secret');
    assert.equal(callback.status, 200);
    assert.match(callback.headers.get('content-type'), /text\/html/);
    assert.ok(!(await callback.text()).includes('secret'));
  } finally {
    await new Promise(resolve => server.close(resolve));
  }
});
