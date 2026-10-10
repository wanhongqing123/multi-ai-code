'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const { signArkRequest, callArk } = require('./ark-sign');

test('matches the official SDK signing result for a fixed Ark request', () => {
  const body = JSON.stringify({ Name: 'figure', GroupType: 'AIGC', ProjectName: 'default' });
  const signed = signArkRequest('CreateAssetGroup', body,
    { accessKeyId: 'TEST_AK', secretKey: 'TEST_SK' }, new Date('2026-10-08T07:00:00Z'));
  assert.equal(signed.url,
    'https://ark.cn-beijing.volcengineapi.com/?Action=CreateAssetGroup&Version=2024-01-01');
  assert.equal(signed.headers['X-Content-Sha256'],
    'baf97701d829ea6fd88bf5192ffae02a34d189d1e8e2a3aa1d15a75907570728');
  assert.equal(signed.headers.Authorization,
    'HMAC-SHA256 Credential=TEST_AK/20261008/cn-beijing/ark/request, ' +
    'SignedHeaders=host;x-content-sha256;x-date, ' +
    'Signature=a09eb822612bd7031692a16e5823dafb585cb2c0cb026ae3c767a27b6f89ae9f');
});

test('sends only to fixed Ark endpoint and parses provider response', async () => {
  const response = await callArk('GetAsset', { Id: 'asset-1234567890' },
    { accessKeyId: 'TEST_AK', secretKey: 'TEST_SK' }, async (url, request) => {
      assert.equal(new URL(url).host, 'ark.cn-beijing.volcengineapi.com');
      assert.equal(request.method, 'POST');
      assert.ok(request.headers.Authorization.startsWith('HMAC-SHA256 Credential=TEST_AK/'));
      assert.equal(JSON.parse(request.body).Id, 'asset-1234567890');
      return { text: async () => JSON.stringify({ Result: { Status: 'Active' } }) };
    });
  assert.equal(response.Result.Status, 'Active');
});

test('preserves Ark HTTP status and request headers on invalid upstream responses', async () => {
  await assert.rejects(
    callArk('ListAssetGroups', {}, { accessKeyId: 'TEST_AK', secretKey: 'TEST_SK' },
      async () => ({ ok: false, status: 503,
        headers: { get: name => ({ 'x-request-id': 'req-123',
          'x-error-code': 'ServiceUnavailable' })[name] || '' },
        text: async () => 'temporarily unavailable' })),
    error => error.response.status === 503 &&
      error.response.data.ResponseMetadata.RequestId === 'req-123' &&
      error.response.data.ResponseMetadata.Error.Code === 'ServiceUnavailable'
  );
});
