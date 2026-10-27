'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const { purgeAccountAssets, main } = require('./purge-account-assets');

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

test('private cloud purge includes exact owner objects and incomplete multipart uploads', async () => {
  const deleted = [];
  const aborted = [];
  const key = 'cloud-drive/test_account/2026-10-24/video.mov';
  const attachmentKey = 'session-attachments/test_account/2026-10-24/file.pdf';
  const client = {
    async list(options) {
      assert.notEqual(options.prefix,'cloud-drive/test_account/');
      return { objects:[] };
    },
    async deleteMulti() { throw new Error('Media bucket must not delete cloud data'); }
  };
  const cloudClient = {
    async list(options) {
      return { objects: options.prefix === 'cloud-drive/test_account/' ?
        [{ name:key }] : options.prefix === 'session-attachments/test_account/' ?
        [{ name:attachmentKey }] : [] };
    },
    async listUploads(options) {
      assert.ok(['cloud-drive/test_account/',
        'session-attachments/test_account/'].includes(options.prefix));
      return { uploads:[{ name:options.prefix.startsWith('cloud-drive/') ?
        key : attachmentKey,uploadId:'upload_123' }],isTruncated:false };
    },
    async deleteMulti(keys) { deleted.push(...keys); },
    async abortMultipartUpload(name,id) { aborted.push([name,id]); }
  };
  const defaultResult = await purgeAccountAssets(client,'test_account',true);
  assert.equal(defaultResult.object_counts['cloud-drive'],undefined);
  const preview = await purgeAccountAssets(client,'test_account',false,false,cloudClient);
  assert.equal(preview.object_counts['cloud-drive'],1);
  assert.equal(preview.object_counts['session-attachments'],1);
  assert.equal(preview.multipart_upload_counts['cloud-drive'],1);
  assert.equal(preview.multipart_upload_counts['session-attachments'],1);
  assert.deepEqual(aborted,[]);
  const executed = await purgeAccountAssets(client,'test_account',true,false,cloudClient);
  assert.equal(executed.object_counts['cloud-drive'],1);
  assert.equal(executed.object_counts['session-attachments'],1);
  assert.deepEqual(deleted,[key,attachmentKey]);
  assert.deepEqual(aborted,[[key,'upload_123'],[attachmentKey,'upload_123']]);
  await assert.rejects(purgeAccountAssets(client,
    'test_account',false,false,{
    list:async () => ({ objects:[] }),
    listUploads:async () => ({ uploads:[{ name:'cloud-drive/other_user/file.mp4',
      uploadId:'upload_999' }],isTruncated:false })
  }),/outside the account prefix/);
});

test('cloud purge command requires separate bucket and credentials', async () => {
  const media = { MAICHAT_OSS_BUCKET:'media-bucket',
    MAICHAT_OSS_REGION:'oss-cn-guangzhou',
    ALIBABA_CLOUD_ACCESS_KEY_ID:'media-id',
    ALIBABA_CLOUD_ACCESS_KEY_SECRET:'media-secret' };
  await assert.rejects(main(['--include-cloud-drive','test_account'],media),
    /with OSS credentials/);
  await assert.rejects(main(['--include-cloud-drive','test_account'],{
    ...media,
    MAICHAT_CLOUD_DRIVE_OSS_BUCKET:'media-bucket',
    MAICHAT_CLOUD_DRIVE_OSS_REGION:'oss-cn-guangzhou',
    MAICHAT_CLOUD_DRIVE_OSS_ACCESS_KEY_ID:'cloud-id',
    MAICHAT_CLOUD_DRIVE_OSS_ACCESS_KEY_SECRET:'cloud-secret'
  }),/with OSS credentials/);
});
