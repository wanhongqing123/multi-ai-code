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

test('circle objects are included only by an explicit account cleanup option', async () => {
  const deleted = [];
  const client = {
    async list(options) {
      return { objects: options.prefix === 'video-circle/test_account/' ?
        [{ name: 'video-circle/test_account/2026-10-22/clip.mp4' }] : [] };
    },
    async deleteMulti(keys) { deleted.push(...keys); }
  };
  const ordinary = await purgeAccountAssets(client, 'test_account', true);
  assert.equal(ordinary.object_counts['video-circle'], undefined);
  const circle = await purgeAccountAssets(client, 'test_account', true, true);
  assert.equal(circle.object_counts['video-circle'], 1);
  assert.deepEqual(deleted, ['video-circle/test_account/2026-10-22/clip.mp4']);
});
