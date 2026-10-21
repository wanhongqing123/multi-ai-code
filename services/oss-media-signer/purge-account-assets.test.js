'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const { purgeAccountAssets } = require('./purge-account-assets');

test('account asset purge only touches keys below the exact owner prefix', async () => {
  const deleted = [];
  const client = {
    async list(options) {
      assert.equal(Object.hasOwn(options, 'marker'), false);
      return { objects: options.prefix === 'avatars/test_account/' ?
        [{ name: 'avatars/test_account/2026-10-20/photo.jpg' }] : [] };
    },
    async deleteMulti(keys) { deleted.push(...keys); }
  };
  const preview = await purgeAccountAssets(client, 'test_account');
  assert.equal(preview.object_counts.avatars, 1);
  assert.deepEqual(deleted, []);
  const result = await purgeAccountAssets(client, 'test_account', true);
  assert.equal(result.execute, true);
  assert.deepEqual(deleted, ['avatars/test_account/2026-10-20/photo.jpg']);
  await assert.rejects(purgeAccountAssets({
    list: async () => ({ objects: [{ name: 'avatars/other_user/file.jpg' }] })
  }, 'test_account', true), /outside the requested account prefix/);
});
