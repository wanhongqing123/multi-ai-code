'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const { createServer } = require('./server');

test('HTTP adapter rejects oversized and invalid requests without invoking auth', async () => {
  let calls = 0;
  const server = createServer({ async handle() {
    ++calls;
    return { statusCode: 200, body: { ok: true } };
  } });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  try {
    const url = `http://127.0.0.1:${server.address().port}/v1/auth/sms/challenge`;
    const invalid = await fetch(url, { method: 'POST', body: '{' });
    assert.equal(invalid.status, 400);
    assert.equal(invalid.headers.get('cache-control'), 'no-store');
    const oversized = await fetch(url, { method: 'POST', body: 'x'.repeat(4097) });
    assert.equal(oversized.status, 413);
    assert.equal(calls, 0);
    const valid = await fetch(url, { method: 'POST', body: '{}' });
    assert.equal(valid.status, 200);
    assert.equal(calls, 1);
  } finally {
    await new Promise(resolve => server.close(resolve));
  }
});
