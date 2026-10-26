'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const { newCredential, allowlistMatchesPrimary, validateCanaryConfiguration } =
  require('./cloud-drive-production-canary-accounts');

test('canary credentials have separate valid random identities and passwords', () => {
  const a = newCredential('a');
  const b = newCredential('b');
  assert.match(a.id, /^cloudcanary_a_[0-9a-f]{12}$/);
  assert.match(b.id, /^cloudcanary_b_[0-9a-f]{12}$/);
  assert.notEqual(a.id, b.id);
  for (const account of [a,b]) {
    assert.match(account.password, /^[A-Za-z0-9_-]{48}$/);
    assert.equal(account.state, 'prepared');
  }
  assert.throws(() => newCredential('c'));
});

test('canary allowlist permits only account A', () => {
  const a = newCredential('a');
  const b = newCredential('b');
  assert.equal(allowlistMatchesPrimary(a.id, a.id), true);
  assert.equal(allowlistMatchesPrimary(` ${a.id} `, a.id), true);
  for (const value of [undefined, '', '*', b.id, `${a.id},${b.id}`,
    `${b.id},${a.id}`])
    assert.equal(allowlistMatchesPrimary(value, a.id), false);
});

test('canary provision requires both private config sources and the running service', () => {
  const a = newCredential('a');
  const files = '/etc/maichat/auth.env (ignore_errors=no)\n' +
    '/etc/maichat/cloud-drive-auth-canary.env (ignore_errors=no)\n';
  const base = { MAICHAT_AUTH_ADMIN_SECRET: 'a'.repeat(32),
    MAICHAT_AUTH_ADMIN_PORT:'9101', MAICHAT_AUTH_PORT:'9100' };
  const canary = { MAICHAT_CLOUD_DRIVE_ENABLED:'1',
    MAICHAT_CLOUD_DRIVE_ALLOWED_ACCOUNTS:a.id,
    MAICHAT_CLOUD_DRIVE_INTERNAL_KEY:'b'.repeat(32) };
  const live = { ...base, ...canary };
  assert.deepEqual(validateCanaryConfiguration(base,canary,live,files,a.id),
    { ...base,...canary });
  assert.throws(() => validateCanaryConfiguration(base,canary,live,
    '/etc/maichat/auth.env (ignore_errors=no)\n',a.id));
  assert.throws(() => validateCanaryConfiguration(base,
    { ...canary, MAICHAT_AUTH_ADMIN_SECRET:'c'.repeat(32) },live,files,a.id));
  assert.throws(() => validateCanaryConfiguration(base,
    { ...canary, MAICHAT_CLOUD_DRIVE_ALLOWED_ACCOUNTS:`${a.id},other` },
    live,files,a.id));
  assert.throws(() => validateCanaryConfiguration(base,canary,
    { ...live, MAICHAT_CLOUD_DRIVE_ENABLED:'0' },files,a.id));
  assert.throws(() => validateCanaryConfiguration(base,canary,
    { ...live, MAICHAT_CLOUD_DRIVE_ALLOWED_ACCOUNTS:'*' },files,a.id));
  assert.throws(() => validateCanaryConfiguration(base,canary,
    { ...live, MAICHAT_CLOUD_DRIVE_INTERNAL_KEY:'different' },files,a.id));
});
