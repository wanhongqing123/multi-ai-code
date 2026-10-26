'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const Database = require('better-sqlite3');
const { openStore } = require('./store');
const { createCloudDriveService } = require('./cloud-drive-service');
const { completeAccountDeletion } = require('./account-deletion');

const PART_BYTES = 8*1024*1024;
const MD5 = Buffer.alloc(16,1).toString('base64');

function fixture() {
  const db = openStore(':memory:');
  let time = Date.parse('2026-10-24T00:00:00Z');
  const owner = { user_id:'user-a',account_id:'account_a' };
  const other = { user_id:'user-b',account_id:'account_b' };
  for (const user of [owner,other]) {
    db.prepare(`INSERT INTO users(id,account_id,im_user_id,created_at,updated_at)
      VALUES(?,?,?,?,?)`).run(user.user_id,user.account_id,user.account_id,time,time);
  }
  const storage = new Map();
  const activeUploads = new Map();
  const listed = [];
  const aborted = [];
  const deleted = [];
  let uploads = 0;
  let failCleanup = false;
  let failList = false;
  let ignoreAbort = false;
  let loseStartResponse = false;
  let ignoreDelete = false;
  let completionGate = null;
  let verificationGate = null;
  let startGate = null;
  const service = createCloudDriveService({ db,now:() => time,
    startUpload:async (_account,key) => {
      const uploadId = `upload_${++uploads}`;
      storage.set(key,{ parts:[],bytes:null,type:null });
      activeUploads.set(uploadId,key);
      if (startGate) {
        startGate.entered();
        await startGate.promise;
      }
      return loseStartResponse ? undefined : uploadId;
    },
    signPart:async (_account,key,uploadId,number,bytes,md5) => ({
      upload_url:`https://test-bucket.oss-cn-hangzhou.aliyuncs.com/${key}` +
        `?partNumber=${number}&uploadId=${uploadId}`,
      upload_headers:{ 'Content-Length':String(bytes),'Content-MD5':md5 }
    }),
    listParts:async (_account,key) => storage.get(key).parts,
    listUploads:async (_account,key) => {
      if (failList) throw new Error('temporary OSS listing failure');
      listed.push(key);
      return [...activeUploads].filter(([,activeKey]) => activeKey === key)
        .map(([uploadId]) => uploadId);
    },
    completeUpload:async (_account,key,_uploadId,parts) => {
      if (completionGate) {
        completionGate.entered();
        await completionGate.promise;
      }
      const object = storage.get(key) || { parts };
      object.bytes = parts.reduce((sum,part) => sum+part.size_bytes,0);
      object.type = key.endsWith('.mov') ? 'video/quicktime' :
        key.endsWith('.mp4') ? 'video/mp4' :
        key.endsWith('.png') ? 'image/png' :
        key.endsWith('.webp') ? 'image/webp' :
        key.endsWith('.heic') ? 'image/heic' : 'image/jpeg';
      assert.deepEqual(parts,object.parts);
      storage.set(key,object);
      activeUploads.delete(_uploadId);
    },
    abortUpload:async (_account,key,uploadId) => {
      if (failCleanup) throw new Error('temporary OSS failure');
      aborted.push([key,uploadId]);
      if (!ignoreAbort) activeUploads.delete(uploadId);
    },
    verifyObject:async (_account,key) => {
      if (verificationGate) {
        const gate = verificationGate;
        verificationGate = null;
        gate.entered();
        await gate.promise;
      }
      const object = storage.get(key);
      return object?.bytes == null ? { exists:false } :
        { exists:true,size_bytes:object.bytes,content_type:object.type };
    },
    signRead:async (_account,key) => `https://test-bucket.oss-cn-hangzhou.aliyuncs.com/${key}?read=1`,
    signThumbnail:async (_account,key) =>
      `https://test-bucket.oss-cn-hangzhou.aliyuncs.com/${key}?thumbnail=1`,
    deleteObject:async (_account,key) => {
      if (failCleanup) throw new Error('temporary OSS failure');
      deleted.push(key);
      if (!ignoreDelete) storage.delete(key);
    }
  });
  const call = (principal,method,path,body = {},query) =>
    service.handle(method,path,body,principal,query);
  return { db,storage,activeUploads,listed,aborted,deleted,service,owner,other,call,
    advance:milliseconds => { time += milliseconds; },
    failCleanup:value => { failCleanup = value; },
    failList:value => { failList = value; },
    ignoreAbort:value => { ignoreAbort = value; },
    loseStartResponse:value => { loseStartResponse = value; },
    ignoreDelete:value => { ignoreDelete = value; },
    deferComplete:() => {
      let release,enter;
      const promise = new Promise(resolve => { release = resolve; });
      const entered = new Promise(resolve => { enter = resolve; });
      completionGate = { promise,entered:enter };
      return { entered,release };
    },
    deferStart:() => {
      let release,enter;
      const promise = new Promise(resolve => { release = resolve; });
      const entered = new Promise(resolve => { enter = resolve; });
      startGate = { promise,entered:enter };
      return { entered,release };
    },
    deferVerifyOnce:() => {
      let release,enter;
      const promise = new Promise(resolve => { release = resolve; });
      const entered = new Promise(resolve => { enter = resolve; });
      verificationGate = { promise,entered:enter };
      return { entered,release };
    } };
}

test('account allowlist hides API but never blocks account cleanup', async t => {
  const f = fixture();
  t.after(() => f.db.close());
  const created = await f.call(f.owner,'POST','/v1/cloud-drive/videos/uploads',
    { name:'old.mp4',size_bytes:1000,content_type:'video/mp4' });
  assert.equal(created.statusCode,201);
  const key = f.db.prepare('SELECT object_key FROM cloud_drive_videos WHERE id=?')
    .get(created.body.video_id).object_key;
  let aborted = 0;
  let deleted = 0;
  const canary = createCloudDriveService({ db:f.db,
    now:() => Date.parse('2026-10-24T00:00:00Z'),
    allowedAccountIds:new Set([f.other.account_id]),
    abortUpload:async () => { aborted++; },
    listUploads:async () => [],
    deleteObject:async () => { deleted++; f.storage.delete(key); },
    verifyObject:async () => ({ exists:false }) });
  assert.equal((await canary.handle('GET','/v1/cloud-drive/videos',{},f.owner))
    .statusCode,404);
  assert.equal((await canary.handle('DELETE',
    `/v1/cloud-drive/videos/${created.body.video_id}`,{},f.owner)).statusCode,404);
  assert.equal((await canary.cleanupAccount(f.owner.user_id)).pending,0);
  assert.equal(aborted,1);
  assert.equal(deleted,1);
  assert.equal(f.storage.has(key),false);
  assert.equal(f.db.prepare('SELECT 1 FROM cloud_drive_videos WHERE id=?')
    .get(created.body.video_id),undefined);
});

test('private resumable upload, authoritative parts, playback and removal', async t => {
  const f = fixture();
  t.after(() => f.db.close());
  const created = await f.call(f.owner,'POST','/v1/cloud-drive/videos/uploads',
    { name:'holiday.MOV',size_bytes:PART_BYTES+6,content_type:'video/quicktime' });
  assert.equal(created.statusCode,201);
  const { video_id:id,upload_id:uploadId,upload_expires_at:expires } = created.body;
  assert.equal(created.body.part_size,PART_BYTES);
  assert.equal(created.body.parts_total,2);
  assert.equal(expires,'2026-10-31T00:00:00.000Z');
  assert.equal(created.body.quota.reserved_bytes,PART_BYTES+6);
  assert.equal((await f.call(f.other,'GET',`/v1/cloud-drive/videos/uploads/${id}/parts`))
    .statusCode,404);
  assert.equal((await f.call(f.other,'GET',`/v1/cloud-drive/videos/${id}/play-link`))
    .statusCode,404);
  assert.equal((await f.call(f.owner,'POST',
    `/v1/cloud-drive/videos/uploads/${id}/complete`)).statusCode,409);
  const key = f.db.prepare('SELECT object_key FROM cloud_drive_videos WHERE id=?').get(id)
    .object_key;
  const first = await f.call(f.owner,'POST',
    `/v1/cloud-drive/videos/uploads/${id}/parts/1/sign`,{ content_md5_base64:MD5 });
  assert.equal(first.statusCode,200);
  assert.equal(first.body.upload_headers['Content-Length'],String(PART_BYTES));
  assert.equal(first.body.upload_headers['Content-MD5'],MD5);
  assert.match(first.body.upload_url,new RegExp(`uploadId=${uploadId}$`));
  f.storage.get(key).parts.push({ part_number:1,size_bytes:PART_BYTES,
    etag:'"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"' });
  assert.deepEqual((await f.call(f.owner,'GET',
    `/v1/cloud-drive/videos/uploads/${id}/parts`)).body.parts,
    f.storage.get(key).parts);
  const last = await f.call(f.owner,'POST',
    `/v1/cloud-drive/videos/uploads/${id}/parts/2/sign`,{ content_md5_base64:MD5 });
  assert.equal(last.body.part_size_bytes,6);
  f.storage.get(key).parts.push({ part_number:2,size_bytes:6,
    etag:'"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"' });
  const done = await f.call(f.owner,'POST',
    `/v1/cloud-drive/videos/uploads/${id}/complete`);
  assert.equal(done.statusCode,200);
  assert.equal(done.body.status,'ready');
  assert.equal((await f.call(f.owner,'POST',
    `/v1/cloud-drive/videos/uploads/${id}/complete`)).statusCode,200);
  assert.equal((await f.call(f.owner,'GET',
    `/v1/cloud-drive/videos/uploads/${id}/parts`)).body.status,'ready');
  const ownList = await f.call(f.owner,'GET','/v1/cloud-drive/videos');
  assert.equal(ownList.body.videos.length,1);
  assert.equal(ownList.body.quota.used_bytes,PART_BYTES+6);
  assert.equal(ownList.body.quota.reserved_bytes,0);
  assert.deepEqual((await f.call(f.other,'GET','/v1/cloud-drive/videos')).body.videos,[]);
  const link = await f.call(f.owner,'GET',`/v1/cloud-drive/videos/${id}/play-link`);
  assert.equal(link.statusCode,200);
  assert.equal(link.body.read_expires_in,60);
  assert.equal(link.body.size_bytes,PART_BYTES+6);
  assert.equal(link.body.content_type,'video/quicktime');
  assert.equal((await f.call(f.other,'DELETE',`/v1/cloud-drive/videos/${id}`))
    .statusCode,404);
  assert.equal((await f.call(f.owner,'DELETE',`/v1/cloud-drive/videos/${id}`))
    .statusCode,200);
  assert.deepEqual(f.deleted,[key]);
  assert.equal(f.activeUploads.has(uploadId),false);
  assert.equal(f.storage.has(key),false);
  assert.equal(f.db.prepare('SELECT 1 FROM cloud_drive_videos WHERE id=?').get(id),
    undefined);
  assert.equal((await f.call(f.owner,'GET','/v1/cloud-drive/videos')).body.quota.used_bytes,0);
});

test('private images share multipart quota and expose account-bound thumbnails', async t => {
  const f = fixture();
  t.after(() => f.db.close());
  const formats = [
    ['portrait.JPG','image/jpeg','.jpg'],
    ['portrait.jpeg','image/jpeg','.jpeg'],
    ['diagram.png','image/png','.png'],
    ['preview.webp','image/webp','.webp'],
    ['iPhone.heic','image/heic','.heic']
  ];
  for (const [name,contentType,extension] of formats) {
    const created = await f.call(f.owner,'POST','/v1/cloud-drive/videos/uploads',
      { name,size_bytes:1234,content_type:contentType });
    assert.equal(created.statusCode,201,name);
    assert.equal(created.body.media_type,'image');
    const id = created.body.video_id;
    const key = f.db.prepare('SELECT object_key FROM cloud_drive_videos WHERE id=?')
      .get(id).object_key;
    assert.ok(key.endsWith(extension));
    assert.equal((await f.call(f.other,'GET',
      `/v1/cloud-drive/videos/${id}/thumbnail-link`)).statusCode,404);
    const signed = await f.call(f.owner,'POST',
      `/v1/cloud-drive/videos/uploads/${id}/parts/1/sign`,
      { content_md5_base64:MD5 });
    assert.equal(signed.statusCode,200);
    assert.equal(signed.body.part_size_bytes,1234);
    f.storage.get(key).parts.push({ part_number:1,size_bytes:1234,
      etag:'"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"' });
    const completed = await f.call(f.owner,'POST',
      `/v1/cloud-drive/videos/uploads/${id}/complete`);
    assert.equal(completed.statusCode,200);
    assert.equal(completed.body.media_type,'image');
    const thumbnail = await f.call(f.owner,'GET',
      `/v1/cloud-drive/videos/${id}/thumbnail-link`);
    assert.equal(thumbnail.statusCode,200);
    assert.equal(thumbnail.body.thumbnail_expires_in,60);
    assert.equal(thumbnail.body.content_type,'image/jpeg');
    assert.equal((await f.call(f.other,'GET',
      `/v1/cloud-drive/videos/${id}/play-link`)).statusCode,404);
  }
  const ownList = await f.call(f.owner,'GET','/v1/cloud-drive/videos');
  assert.equal(ownList.body.videos.length,formats.length);
  assert.ok(ownList.body.videos.every(item => item.media_type === 'image'));
  assert.equal(ownList.body.quota.used_bytes,formats.length*1234);
  assert.deepEqual((await f.call(f.other,'GET','/v1/cloud-drive/videos'))
    .body.videos,[]);
  const first = ownList.body.videos[0].id;
  assert.equal((await f.call(f.owner,'DELETE',
    `/v1/cloud-drive/videos/${first}`)).statusCode,200);
  assert.equal((await f.call(f.owner,'GET',
    `/v1/cloud-drive/videos/${first}/thumbnail-link`)).statusCode,404);
  assert.equal((await f.call(f.owner,'GET','/v1/cloud-drive/videos'))
    .body.quota.used_bytes,(formats.length-1)*1234);
});

test('image MIME and 20 MB cap reject malformed files without reserving quota', async t => {
  const f = fixture();
  t.after(() => f.db.close());
  for (const upload of [
    { name:'wrong.jpg',size_bytes:1,content_type:'image/png' },
    { name:'oversize.png',size_bytes:20_000_001,content_type:'image/png' },
    { name:'movie.mp4',size_bytes:2_000_000_001,content_type:'video/mp4' },
    { name:'unsafe.svg',size_bytes:100,content_type:'image/svg+xml' }
  ]) assert.equal((await f.call(f.owner,'POST',
    '/v1/cloud-drive/videos/uploads',upload)).statusCode,400);
  assert.deepEqual((await f.call(f.owner,'GET','/v1/cloud-drive/videos'))
    .body.quota,{ used_bytes:0,reserved_bytes:0,limit_bytes:10_000_000_000 });
});

test('old video schema migrates active uploads and tombstones without changing IDs', t => {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(),'cloud-media-schema-'));
  t.after(() => fs.rmSync(directory,{ recursive:true,force:true }));
  const filename = path.join(directory,'auth.sqlite');
  const old = new Database(filename);
  old.exec(`CREATE TABLE users (
    id TEXT PRIMARY KEY,account_id TEXT UNIQUE,nickname TEXT,phone_e164 TEXT UNIQUE,
    im_user_id TEXT NOT NULL UNIQUE,password_hash TEXT,created_at INTEGER NOT NULL,
    updated_at INTEGER NOT NULL,disabled_at INTEGER
  );
  CREATE TABLE cloud_drive_videos (
    id TEXT PRIMARY KEY,owner_user_id TEXT NOT NULL REFERENCES users(id),
    name TEXT NOT NULL,object_key TEXT NOT NULL UNIQUE,upload_id TEXT,
    size_bytes INTEGER NOT NULL,
    content_type TEXT NOT NULL CHECK(content_type IN ('video/mp4','video/quicktime')),
    status TEXT NOT NULL CHECK(status IN ('uploading','completing','ready','deleted')),
    created_at INTEGER NOT NULL,upload_expires_at INTEGER NOT NULL,ready_at INTEGER,
    deleted_at INTEGER,cleanup_pending INTEGER NOT NULL DEFAULT 0,
    cleanup_after INTEGER,cleanup_hold_until INTEGER,
    cleanup_revision INTEGER NOT NULL DEFAULT 0,last_part_signed_at INTEGER,
    completion_started_at INTEGER
  );`);
  old.prepare(`INSERT INTO users(id,account_id,im_user_id,created_at,updated_at)
    VALUES('owner','account_a','account_a',1,1)`).run();
  old.prepare(`INSERT INTO cloud_drive_videos
    (id,owner_user_id,name,object_key,upload_id,size_bytes,content_type,
    status,created_at,upload_expires_at,cleanup_pending,cleanup_revision)
    VALUES(?,?,?,?,?,?,?,?,?,?,?,?)`).run(
      '123e4567-e89b-42d3-a456-426614174000','owner','old.mp4',
      'cloud-drive/account_a/2026-10-24/123e4567-e89b-42d3-a456-426614174000.mp4',
      'old_upload_1',8000000,'video/mp4','uploading',1,10000000,0,7);
  old.prepare(`INSERT INTO cloud_drive_videos
    (id,owner_user_id,name,object_key,size_bytes,content_type,status,
    created_at,upload_expires_at,deleted_at,cleanup_pending,cleanup_after,
    cleanup_hold_until,cleanup_revision)
    VALUES(?,?,?,?,?,?,'deleted',?,?,?,?,?,?,?)`).run(
      '123e4567-e89b-42d3-a456-426614174002','owner','deleted.mov',
      'cloud-drive/account_a/2026-10-24/123e4567-e89b-42d3-a456-426614174002.mov',
      5678,'video/quicktime',1,10000000,3,1,5,1000,8);
  old.close();
  const migrated = openStore(filename);
  const row = migrated.prepare(`SELECT * FROM cloud_drive_videos
    WHERE status='uploading'`).get();
  assert.equal(row.upload_id,'old_upload_1');
  assert.equal(row.cleanup_revision,7);
  assert.equal(row.status,'uploading');
  const tombstone = migrated.prepare(`SELECT * FROM cloud_drive_videos
    WHERE status='deleted'`).get();
  assert.equal(tombstone.cleanup_pending,1);
  assert.equal(tombstone.cleanup_hold_until,1000);
  assert.equal(tombstone.cleanup_revision,8);
  assert.equal(migrated.pragma('foreign_key_check').length,0);
  migrated.prepare(`INSERT INTO cloud_drive_videos
    (id,owner_user_id,name,object_key,size_bytes,content_type,status,
    created_at,upload_expires_at) VALUES(?,?,?,?,?,?,'ready',?,?)`).run(
      '123e4567-e89b-42d3-a456-426614174001','owner','new.heic',
      'cloud-drive/account_a/2026-10-24/123e4567-e89b-42d3-a456-426614174001.heic',
      1000,'image/heic',2,10000000);
  migrated.close();
  const reopened = openStore(filename);
  assert.equal(reopened.prepare('SELECT COUNT(*) AS count FROM cloud_drive_videos')
    .get().count,3);
  reopened.close();
});

test('2 GB file limit and 10 GB account reservation are atomic across concurrent init', async t => {
  const f = fixture();
  t.after(() => f.db.close());
  const invalid = await f.call(f.owner,'POST','/v1/cloud-drive/videos/uploads',
    { name:'oversized.mp4',size_bytes:2_000_000_001,content_type:'video/mp4' });
  assert.equal(invalid.statusCode,400);
  const starts = await Promise.all(Array.from({length:6},(_,i) =>
    f.call(f.owner,'POST','/v1/cloud-drive/videos/uploads',
      { name:`video-${i}.mp4`,size_bytes:2_000_000_000,
        content_type:'video/mp4' })));
  assert.deepEqual(starts.map(item => item.statusCode).sort(),
    [201,201,201,201,201,413]);
  const quota = (await f.call(f.owner,'GET','/v1/cloud-drive/videos')).body.quota;
  assert.deepEqual(quota,{ used_bytes:0,reserved_bytes:10_000_000_000,
    limit_bytes:10_000_000_000 });
  const id = starts.find(item => item.statusCode === 201).body.video_id;
  assert.equal((await f.call(f.owner,'DELETE',
    `/v1/cloud-drive/videos/uploads/${id}`)).statusCode,200);
  assert.equal((await f.call(f.owner,'GET','/v1/cloud-drive/videos'))
    .body.quota.reserved_bytes,8_000_000_000);
  assert.equal((await f.call(f.owner,'POST','/v1/cloud-drive/videos/uploads',
    { name:'replacement.mp4',size_bytes:2_000_000_000,
      content_type:'video/mp4' })).statusCode,201);
});

test('expiry, cancel failure and account cleanup retain retryable storage work', async t => {
  const f = fixture();
  t.after(() => f.db.close());
  const created = await f.call(f.owner,'POST','/v1/cloud-drive/videos/uploads',
    { name:'video.mp4',size_bytes:1000,content_type:'video/mp4' });
  const id = created.body.video_id;
  f.failCleanup(true);
  const removed = await f.call(f.owner,'DELETE',
    `/v1/cloud-drive/videos/uploads/${id}`);
  assert.equal(removed.statusCode,202);
  assert.equal(removed.body.cleanup_pending,true);
  assert.equal((await f.call(f.owner,'GET','/v1/cloud-drive/videos'))
    .body.quota.reserved_bytes,1000);
  f.failCleanup(false);
  assert.equal((await f.service.sweep()).cleaned,1);
  assert.equal(f.db.prepare('SELECT 1 FROM cloud_drive_videos WHERE id=?').get(id),
    undefined);
  assert.equal((await f.call(f.owner,'GET','/v1/cloud-drive/videos'))
    .body.quota.reserved_bytes,0);
  const pending = await f.call(f.owner,'POST','/v1/cloud-drive/videos/uploads',
    { name:'next.mp4',size_bytes:1000,content_type:'video/mp4' });
  f.advance(7*24*60*60_000+1);
  assert.equal((await f.call(f.owner,'GET',
    `/v1/cloud-drive/videos/uploads/${pending.body.video_id}/parts`)).statusCode,410);
  assert.equal((await f.service.sweep()).cleaned,1);
  assert.equal((await f.call(f.owner,'GET','/v1/cloud-drive/videos'))
    .body.quota.reserved_bytes,0);
});

test('object still present after DELETE keeps metadata and quota until HEAD confirms absence', async t => {
  const f = fixture();
  t.after(() => f.db.close());
  const created = await f.call(f.owner,'POST','/v1/cloud-drive/videos/uploads',
    { name:'stubborn.mp4',size_bytes:1000,content_type:'video/mp4' });
  const id = created.body.video_id;
  const key = f.db.prepare('SELECT object_key FROM cloud_drive_videos WHERE id=?')
    .get(id).object_key;
  f.storage.get(key).parts.push({ part_number:1,size_bytes:1000,
    etag:'"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"' });
  assert.equal((await f.call(f.owner,'POST',
    `/v1/cloud-drive/videos/uploads/${id}/complete`)).statusCode,200);
  f.ignoreDelete(true);
  assert.equal((await f.call(f.owner,'DELETE',`/v1/cloud-drive/videos/${id}`))
    .statusCode,202);
  assert.equal(f.db.prepare('SELECT cleanup_pending FROM cloud_drive_videos WHERE id=?')
    .get(id).cleanup_pending,1);
  assert.equal((await f.call(f.owner,'GET','/v1/cloud-drive/videos')).body.quota
    .reserved_bytes,1000);
  f.ignoreDelete(false);
  assert.equal((await f.service.sweep()).cleaned,1);
  assert.equal(f.db.prepare('SELECT 1 FROM cloud_drive_videos WHERE id=?').get(id),
    undefined);
  assert.equal((await f.call(f.owner,'GET','/v1/cloud-drive/videos')).body.quota
    .reserved_bytes,0);
});

test('failed exact-key multipart listing keeps the deleted row and quota', async t => {
  const f = fixture();
  t.after(() => f.db.close());
  const created = await f.call(f.owner,'POST','/v1/cloud-drive/videos/uploads',
    { name:'list-failure.mp4',size_bytes:1000,content_type:'video/mp4' });
  const id = created.body.video_id;
  f.failList(true);
  assert.equal((await f.call(f.owner,'DELETE',`/v1/cloud-drive/videos/${id}`))
    .statusCode,202);
  assert.equal(f.db.prepare('SELECT cleanup_pending FROM cloud_drive_videos WHERE id=?')
    .get(id).cleanup_pending,1);
  assert.equal((await f.call(f.owner,'GET','/v1/cloud-drive/videos')).body.quota
    .reserved_bytes,1000);
  f.failList(false);
  assert.equal((await f.service.sweep()).cleaned,1);
  assert.equal(f.db.prepare('SELECT 1 FROM cloud_drive_videos WHERE id=?').get(id),
    undefined);
});

test('a multipart upload still listed after abort keeps metadata and quota', async t => {
  const f = fixture();
  t.after(() => f.db.close());
  const created = await f.call(f.owner,'POST','/v1/cloud-drive/videos/uploads',
    { name:'active.mp4',size_bytes:1000,content_type:'video/mp4' });
  const id = created.body.video_id;
  f.ignoreAbort(true);
  assert.equal((await f.call(f.owner,'DELETE',`/v1/cloud-drive/videos/${id}`))
    .statusCode,202);
  assert.equal(f.activeUploads.has(created.body.upload_id),true);
  assert.equal(f.db.prepare('SELECT cleanup_pending FROM cloud_drive_videos WHERE id=?')
    .get(id).cleanup_pending,1);
  assert.equal((await f.call(f.owner,'GET','/v1/cloud-drive/videos')).body.quota
    .reserved_bytes,1000);
  f.ignoreAbort(false);
  assert.equal((await f.service.sweep()).cleaned,1);
  assert.equal(f.activeUploads.has(created.body.upload_id),false);
  assert.equal(f.db.prepare('SELECT 1 FROM cloud_drive_videos WHERE id=?').get(id),
    undefined);
});

test('account deletion hides active and ready videos and retries failed OSS cleanup', async t => {
  const f = fixture();
  t.after(() => f.db.close());
  const make = name => f.call(f.owner,'POST','/v1/cloud-drive/videos/uploads',
    { name,size_bytes:1000,content_type:'video/mp4' });
  const unfinished = await make('unfinished.mp4');
  const ready = await make('ready.mp4');
  const key = f.db.prepare('SELECT object_key FROM cloud_drive_videos WHERE id=?')
    .get(ready.body.video_id).object_key;
  f.storage.get(key).parts.push({ part_number:1,size_bytes:1000,
    etag:'"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"' });
  assert.equal((await f.call(f.owner,'POST',
    `/v1/cloud-drive/videos/uploads/${ready.body.video_id}/complete`)).statusCode,200);
  f.failCleanup(true);
  const cleanup = await f.service.cleanupAccount(f.owner.user_id);
  assert.equal(cleanup.pending,2);
  assert.equal((await f.call(f.owner,'GET','/v1/cloud-drive/videos')).body.videos.length,0);
  assert.equal((await f.call(f.owner,'GET','/v1/cloud-drive/videos'))
    .body.quota.reserved_bytes,2000);
  f.failCleanup(false);
  assert.equal((await f.service.sweep()).cleaned,2);
  assert.equal((await f.call(f.owner,'GET','/v1/cloud-drive/videos'))
    .body.quota.reserved_bytes,0);
  assert.equal((await f.call(f.owner,'GET',
    `/v1/cloud-drive/videos/uploads/${unfinished.body.video_id}/parts`)).statusCode,404);
});

test('delete during OSS completion does not leave a late object after earlier cleanup', async t => {
  const f = fixture();
  t.after(() => f.db.close());
  const created = await f.call(f.owner,'POST','/v1/cloud-drive/videos/uploads',
    { name:'late.mp4',size_bytes:1000,content_type:'video/mp4' });
  const id = created.body.video_id;
  const key = f.db.prepare('SELECT object_key FROM cloud_drive_videos WHERE id=?')
    .get(id).object_key;
  f.storage.get(key).parts.push({ part_number:1,size_bytes:1000,
    etag:'"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"' });
  const gate = f.deferComplete();
  const completing = f.call(f.owner,'POST',
    `/v1/cloud-drive/videos/uploads/${id}/complete`);
  await gate.entered;
  const removed = await f.call(f.owner,'DELETE',`/v1/cloud-drive/videos/${id}`);
  assert.equal(removed.statusCode,202);
  assert.equal(removed.body.cleanup_pending,true);
  assert.equal(f.storage.has(key),false);
  assert.equal(f.activeUploads.has(created.body.upload_id),false);
  f.advance(60*60_000+1);
  await f.service.sweep();
  assert.equal(f.db.prepare('SELECT cleanup_pending FROM cloud_drive_videos WHERE id=?')
    .get(id).cleanup_pending,1);
  gate.release();
  assert.equal((await completing).statusCode,404);
  assert.equal(f.storage.has(key),false);
  assert.equal(f.activeUploads.has(created.body.upload_id),false);
  assert.equal((await f.call(f.owner,'GET','/v1/cloud-drive/videos'))
    .body.quota.reserved_bytes,1000);
  assert.equal(f.db.prepare('SELECT cleanup_pending FROM cloud_drive_videos WHERE id=?')
    .get(id).cleanup_pending,1);
  f.advance(24*60*60_000+1);
  await f.service.sweep();
  assert.equal(f.db.prepare('SELECT 1 FROM cloud_drive_videos WHERE id=?').get(id),
    undefined);
  assert.equal((await f.call(f.owner,'GET','/v1/cloud-drive/videos'))
    .body.quota.reserved_bytes,0);
});

test('expiry during OSS completion keeps a tombstone until late object is removed', async t => {
  const f = fixture();
  t.after(() => f.db.close());
  const created = await f.call(f.owner,'POST','/v1/cloud-drive/videos/uploads',
    { name:'expired.mp4',size_bytes:1000,content_type:'video/mp4' });
  const id = created.body.video_id;
  const key = f.db.prepare('SELECT object_key FROM cloud_drive_videos WHERE id=?')
    .get(id).object_key;
  f.storage.get(key).parts.push({ part_number:1,size_bytes:1000,
    etag:'"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"' });
  const gate = f.deferComplete();
  const completing = f.call(f.owner,'POST',
    `/v1/cloud-drive/videos/uploads/${id}/complete`);
  await gate.entered;
  f.advance(7*24*60*60_000+1);
  await f.service.sweep();
  assert.equal(f.db.prepare('SELECT cleanup_pending FROM cloud_drive_videos WHERE id=?')
    .get(id).cleanup_pending,1);
  gate.release();
  assert.equal((await completing).statusCode,404);
  assert.equal(f.storage.has(key),false);
  f.advance(24*60*60_000+1);
  await f.service.sweep();
  assert.equal(f.db.prepare('SELECT 1 FROM cloud_drive_videos WHERE id=?').get(id),
    undefined);
});

test('an older cleanup pass cannot clear a newer post-completion tombstone', async t => {
  const f = fixture();
  t.after(() => f.db.close());
  const created = await f.call(f.owner,'POST','/v1/cloud-drive/videos/uploads',
    { name:'revision.mp4',size_bytes:1000,content_type:'video/mp4' });
  const id = created.body.video_id;
  const key = f.db.prepare('SELECT object_key FROM cloud_drive_videos WHERE id=?')
    .get(id).object_key;
  f.storage.get(key).parts.push({ part_number:1,size_bytes:1000,
    etag:'"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"' });
  const completingGate = f.deferComplete();
  const completing = f.call(f.owner,'POST',
    `/v1/cloud-drive/videos/uploads/${id}/complete`);
  await completingGate.entered;
  const verifyGate = f.deferVerifyOnce();
  const deleting = f.call(f.owner,'DELETE',`/v1/cloud-drive/videos/${id}`);
  await verifyGate.entered;
  const beforeRevision = f.db.prepare('SELECT cleanup_revision FROM cloud_drive_videos WHERE id=?')
    .get(id).cleanup_revision;
  completingGate.release();
  assert.equal((await completing).statusCode,404);
  const afterRevision = f.db.prepare('SELECT cleanup_revision FROM cloud_drive_videos WHERE id=?')
    .get(id).cleanup_revision;
  assert.ok(afterRevision > beforeRevision);
  verifyGate.release();
  assert.equal((await deleting).statusCode,202);
  assert.equal(f.db.prepare('SELECT cleanup_pending FROM cloud_drive_videos WHERE id=?')
    .get(id).cleanup_pending,1);
  assert.equal(f.storage.has(key),false);
  f.advance(24*60*60_000+1);
  assert.equal((await f.service.sweep()).cleaned,1);
  assert.equal(f.db.prepare('SELECT 1 FROM cloud_drive_videos WHERE id=?').get(id),
    undefined);
});

test('account deletion cannot finalize during an outstanding OSS completion', async t => {
  const f = fixture();
  t.after(() => f.db.close());
  const created = await f.call(f.owner,'POST','/v1/cloud-drive/videos/uploads',
    { name:'account.mp4',size_bytes:1000,content_type:'video/mp4' });
  const id = created.body.video_id;
  const key = f.db.prepare('SELECT object_key FROM cloud_drive_videos WHERE id=?')
    .get(id).object_key;
  f.storage.get(key).parts.push({ part_number:1,size_bytes:1000,
    etag:'"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"' });
  const gate = f.deferComplete();
  const completing = f.call(f.owner,'POST',
    `/v1/cloud-drive/videos/uploads/${id}/complete`);
  await gate.entered;
  const cleanup = await f.service.cleanupAccount(f.owner.user_id);
  assert.equal(cleanup.pending,1);
  const deletionId = '123e4567-e89b-42d3-a456-426614174001';
  f.db.prepare('UPDATE users SET disabled_at=? WHERE id=?').run(1,f.owner.user_id);
  f.db.prepare(`INSERT INTO account_deletion_requests
    (id,user_id,account_id,im_user_id,requested_at,due_at,status)
    VALUES(?,?,?,?,?,?,'pending')`).run(deletionId,f.owner.user_id,
      f.owner.account_id,f.owner.account_id,1,100000);
  const evidence = { operator:'test_admin',records:{} };
  for (const name of ['tencent_im_deleted','session_sync_deleted',
    'oss_and_ark_assets_reviewed','shared_content_reviewed']) {
    evidence[name] = true;
    evidence.records[name] = `verified-${name}`;
  }
  const finish = () => completeAccountDeletion(f.db,deletionId,evidence,
    'test-only-hmac-key-at-least-32-characters',1000);
  assert.throws(finish,/Account media cleanup is not complete/);
  gate.release();
  assert.equal((await completing).statusCode,404);
  assert.equal(f.storage.has(key),false);
  assert.throws(finish,/Account media cleanup is not complete/);
  f.advance(24*60*60_000+1);
  await f.service.sweep();
  assert.equal(f.db.prepare('SELECT 1 FROM cloud_drive_videos WHERE id=?').get(id),
    undefined);
  assert.equal(finish().status,'completed');
});

test('delete during OSS initialization retains the upload ID for retry and purges metadata last', async t => {
  const f = fixture();
  t.after(() => f.db.close());
  const gate = f.deferStart();
  const starting = f.call(f.owner,'POST','/v1/cloud-drive/videos/uploads',
    { name:'starting.mp4',size_bytes:1000,content_type:'video/mp4' });
  await gate.entered;
  const row = f.db.prepare('SELECT id,object_key FROM cloud_drive_videos WHERE name=?')
    .get('starting.mp4');
  f.failCleanup(true);
  const removed = await f.call(f.owner,'DELETE',`/v1/cloud-drive/videos/${row.id}`);
  assert.equal(removed.statusCode,202);
  assert.equal(removed.body.quota.reserved_bytes,1000);
  assert.equal(f.db.prepare('SELECT upload_id,cleanup_pending FROM cloud_drive_videos WHERE id=?')
    .get(row.id).upload_id,null);
  gate.release();
  assert.equal((await starting).statusCode,409);
  const deleted = f.db.prepare(`SELECT upload_id,cleanup_pending,cleanup_hold_until
    FROM cloud_drive_videos WHERE id=?`).get(row.id);
  assert.equal(deleted.upload_id,'upload_1');
  assert.equal(deleted.cleanup_pending,1);
  assert.ok(deleted.cleanup_hold_until > Date.parse('2026-10-24T00:00:00Z'));
  assert.equal(f.activeUploads.has('upload_1'),true);
  f.failCleanup(false);
  await f.service.sweep();
  assert.equal(f.activeUploads.has('upload_1'),false);
  assert.equal(f.storage.has(row.object_key),false);
  assert.equal(f.db.prepare('SELECT cleanup_pending FROM cloud_drive_videos WHERE id=?')
    .get(row.id).cleanup_pending,1);
  assert.equal((await f.call(f.owner,'GET','/v1/cloud-drive/videos')).body.quota
    .reserved_bytes,1000);
  f.advance(24*60*60_000+1);
  assert.equal((await f.service.sweep()).cleaned,1);
  assert.equal(f.db.prepare('SELECT 1 FROM cloud_drive_videos WHERE id=?').get(row.id),
    undefined);
  assert.equal((await f.call(f.owner,'GET','/v1/cloud-drive/videos')).body.quota
    .reserved_bytes,0);
});

test('an unknown upload ID is reconciled by exact-key listing before metadata purge', async t => {
  const f = fixture();
  t.after(() => f.db.close());
  const created = await f.call(f.owner,'POST','/v1/cloud-drive/videos/uploads',
    { name:'unknown-id.mp4',size_bytes:1000,content_type:'video/mp4' });
  const id = created.body.video_id;
  f.db.prepare(`UPDATE cloud_drive_videos SET status='deleted',upload_id=NULL,
    deleted_at=?,cleanup_pending=1,cleanup_after=?,cleanup_hold_until=NULL
    WHERE id=?`).run(Date.parse('2026-10-24T00:00:00Z'),
      Date.parse('2026-10-24T00:00:00Z'),id);
  await f.service.sweep();
  const row = f.db.prepare(`SELECT cleanup_pending,cleanup_hold_until
    FROM cloud_drive_videos WHERE id=?`).get(id);
  assert.equal(row.cleanup_pending,1);
  assert.equal(row.cleanup_hold_until,
    Date.parse('2026-10-25T00:00:00Z'));
  assert.equal((await f.call(f.owner,'GET','/v1/cloud-drive/videos')).body.quota
    .reserved_bytes,1000);
  assert.equal(f.activeUploads.has(created.body.upload_id),false);
  f.advance(24*60*60_000+1);
  assert.equal((await f.service.sweep()).cleaned,1);
  assert.equal(f.db.prepare('SELECT 1 FROM cloud_drive_videos WHERE id=?').get(id),
    undefined);
  assert.equal((await f.call(f.owner,'GET','/v1/cloud-drive/videos')).body.quota
    .reserved_bytes,0);
});

test('lost OSS init response keeps a tombstone until exact-key cleanup and hold finish', async t => {
  const f = fixture();
  t.after(() => f.db.close());
  f.loseStartResponse(true);
  const result = await f.call(f.owner,'POST','/v1/cloud-drive/videos/uploads',
    { name:'lost-response.mp4',size_bytes:1000,content_type:'video/mp4' });
  assert.equal(result.statusCode,502);
  const row = f.db.prepare(`SELECT id,upload_id,status,cleanup_pending,
    cleanup_hold_until FROM cloud_drive_videos WHERE name=?`)
    .get('lost-response.mp4');
  assert.equal(row.upload_id,null);
  assert.equal(row.status,'deleted');
  assert.equal(row.cleanup_pending,1);
  assert.ok(row.cleanup_hold_until > Date.parse('2026-10-24T00:00:00Z'));
  assert.equal(f.activeUploads.size,0);
  assert.equal((await f.call(f.owner,'GET','/v1/cloud-drive/videos')).body.quota
    .reserved_bytes,1000);
  f.advance(24*60*60_000+1);
  assert.equal((await f.service.sweep()).cleaned,1);
  assert.equal(f.db.prepare('SELECT 1 FROM cloud_drive_videos WHERE id=?')
    .get(row.id),undefined);
  assert.equal((await f.call(f.owner,'GET','/v1/cloud-drive/videos')).body.quota
    .reserved_bytes,0);
});

test('legacy cleanup-complete tombstone is reverified and purged without recharging quota', async t => {
  const f = fixture();
  t.after(() => f.db.close());
  const created = await f.call(f.owner,'POST','/v1/cloud-drive/videos/uploads',
    { name:'legacy.mp4',size_bytes:1000,content_type:'video/mp4' });
  const id = created.body.video_id;
  const key = f.db.prepare('SELECT object_key FROM cloud_drive_videos WHERE id=?')
    .get(id).object_key;
  f.db.prepare(`UPDATE cloud_drive_videos SET status='deleted',
    deleted_at=?,cleanup_pending=0,cleanup_after=NULL,cleanup_hold_until=NULL
    WHERE id=?`).run(Date.parse('2026-10-22T00:00:00Z'),id);
  assert.equal((await f.call(f.owner,'GET','/v1/cloud-drive/videos')).body.quota
    .reserved_bytes,0);
  assert.equal((await f.service.sweep()).cleaned,1);
  assert.deepEqual(f.listed,[key,key]);
  assert.equal(f.activeUploads.has(created.body.upload_id),false);
  assert.equal(f.storage.has(key),false);
  assert.equal(f.db.prepare('SELECT 1 FROM cloud_drive_videos WHERE id=?').get(id),
    undefined);
  assert.equal((await f.call(f.owner,'GET','/v1/cloud-drive/videos')).body.quota
    .reserved_bytes,0);
});

test('legacy tombstone with unverified OSS object becomes pending and reserves quota', async t => {
  const f = fixture();
  t.after(() => f.db.close());
  const created = await f.call(f.owner,'POST','/v1/cloud-drive/videos/uploads',
    { name:'legacy-object.mp4',size_bytes:1000,content_type:'video/mp4' });
  const id = created.body.video_id;
  const key = f.db.prepare('SELECT object_key FROM cloud_drive_videos WHERE id=?')
    .get(id).object_key;
  f.storage.get(key).parts.push({ part_number:1,size_bytes:1000,
    etag:'"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"' });
  assert.equal((await f.call(f.owner,'POST',
    `/v1/cloud-drive/videos/uploads/${id}/complete`)).statusCode,200);
  f.db.prepare(`UPDATE cloud_drive_videos SET status='deleted',
    deleted_at=?,cleanup_pending=0,cleanup_after=NULL,cleanup_hold_until=NULL
    WHERE id=?`).run(Date.parse('2026-10-22T00:00:00Z'),id);
  f.ignoreDelete(true);
  assert.equal((await f.service.sweep()).cleaned,0);
  assert.equal(f.storage.has(key),true);
  assert.equal(f.db.prepare('SELECT cleanup_pending FROM cloud_drive_videos WHERE id=?')
    .get(id).cleanup_pending,1);
  assert.equal((await f.call(f.owner,'GET','/v1/cloud-drive/videos')).body.quota
    .reserved_bytes,1000);
  f.ignoreDelete(false);
  assert.equal((await f.service.sweep()).cleaned,1);
  assert.equal(f.db.prepare('SELECT 1 FROM cloud_drive_videos WHERE id=?').get(id),
    undefined);
});

test('recent legacy tombstone waits a full day before metadata and quota release', async t => {
  const f = fixture();
  t.after(() => f.db.close());
  const created = await f.call(f.owner,'POST','/v1/cloud-drive/videos/uploads',
    { name:'legacy-recent.mp4',size_bytes:1000,content_type:'video/mp4' });
  const id = created.body.video_id;
  f.db.prepare(`UPDATE cloud_drive_videos SET status='deleted',
    deleted_at=?,cleanup_pending=0,cleanup_after=NULL,cleanup_hold_until=NULL
    WHERE id=?`).run(Date.parse('2026-10-24T00:00:00Z'),id);
  assert.equal((await f.service.sweep()).cleaned,0);
  const held = f.db.prepare(`SELECT cleanup_pending,cleanup_hold_until
    FROM cloud_drive_videos WHERE id=?`).get(id);
  assert.equal(held.cleanup_pending,1);
  assert.equal(held.cleanup_hold_until,Date.parse('2026-10-25T00:00:00Z'));
  assert.equal((await f.call(f.owner,'GET','/v1/cloud-drive/videos')).body.quota
    .reserved_bytes,1000);
  f.advance(24*60*60_000+1);
  assert.equal((await f.service.sweep()).cleaned,1);
  assert.equal(f.db.prepare('SELECT 1 FROM cloud_drive_videos WHERE id=?').get(id),
    undefined);
});

test('legacy tombstone cannot be purged ahead of a delayed Complete', async t => {
  const f = fixture();
  t.after(() => f.db.close());
  const created = await f.call(f.owner,'POST','/v1/cloud-drive/videos/uploads',
    { name:'legacy-completion.mp4',size_bytes:1000,content_type:'video/mp4' });
  const id = created.body.video_id;
  const key = f.db.prepare('SELECT object_key FROM cloud_drive_videos WHERE id=?')
    .get(id).object_key;
  f.storage.get(key).parts.push({ part_number:1,size_bytes:1000,
    etag:'"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"' });
  const gate = f.deferComplete();
  const completing = f.call(f.owner,'POST',
    `/v1/cloud-drive/videos/uploads/${id}/complete`);
  await gate.entered;
  f.db.prepare(`UPDATE cloud_drive_videos SET status='deleted',deleted_at=?,
    cleanup_pending=0,cleanup_after=NULL,cleanup_hold_until=NULL WHERE id=?`)
    .run(Date.parse('2026-10-24T00:00:00Z'),id);
  assert.equal((await f.service.sweep()).cleaned,0);
  const before = f.db.prepare(`SELECT cleanup_pending,cleanup_hold_until,
    cleanup_revision FROM cloud_drive_videos WHERE id=?`).get(id);
  assert.equal(before.cleanup_pending,1);
  assert.equal(before.cleanup_hold_until,Date.parse('2026-10-25T00:00:00Z'));
  assert.equal((await f.call(f.owner,'GET','/v1/cloud-drive/videos')).body.quota
    .reserved_bytes,1000);
  gate.release();
  assert.equal((await completing).statusCode,404);
  const after = f.db.prepare('SELECT cleanup_revision FROM cloud_drive_videos WHERE id=?')
    .get(id).cleanup_revision;
  assert.ok(after > before.cleanup_revision);
  assert.equal(f.storage.has(key),false);
  f.advance(24*60*60_000+1);
  assert.equal((await f.service.sweep()).cleaned,1);
  assert.equal(f.db.prepare('SELECT 1 FROM cloud_drive_videos WHERE id=?').get(id),
    undefined);
});
