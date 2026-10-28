'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const { randomUUID,createHash } = require('node:crypto');
const { openStore } = require('./store');
const { createCloudDriveService } = require('./cloud-drive-service');

const VIDEOS = '/v1/cloud-drive/videos';
const TIME = Date.parse('2026-10-28T15:00:00Z');

function fixture(t) {
  const db = openStore(':memory:');
  t.after(() => db.close());
  const owner = { user_id:'rename-owner',account_id:'rename_owner' };
  const other = { user_id:'rename-other',account_id:'rename_other' };
  for (const user of [owner,other])
    db.prepare(`INSERT INTO users(id,account_id,im_user_id,created_at,updated_at)
      VALUES(?,?,?,?,?)`).run(user.user_id,user.account_id,user.account_id,TIME,TIME);
  const objects = new Map();
  const readKeys = [];
  const deletedKeys = [];
  const service = createCloudDriveService({ db,now:() => TIME,
    signRead:async (_account,key) => {
      readKeys.push(key);
      return `https://private-test-bucket.oss-cn-hangzhou.aliyuncs.com/${key}?signature=fixture`;
    },
    signThumbnail:async (_account,key) =>
      `https://private-test-bucket.oss-cn-hangzhou.aliyuncs.com/${key}?thumbnail=fixture`,
    abortUpload:async () => {},listUploads:async () => [],
    deleteObject:async (_account,key) => { deletedKeys.push(key); objects.delete(key); },
    verifyObject:async (_account,key) => ({ exists:objects.has(key) }) });
  const call = (method,route,body = {},principal = owner) =>
    service.handle(method,route,body,principal);
  function media({ name = 'original.mp4',contentType = 'video/mp4',
    status = 'ready',folder = null } = {}) {
    const id = randomUUID();
    const bytes = Buffer.from(`private source media bytes ${id}`);
    const key = `cloud-drive/rename_owner/2026-10-28/${id}${name.slice(name.lastIndexOf('.')).toLowerCase()}`;
    db.prepare(`INSERT INTO cloud_drive_videos
      (id,owner_user_id,name,object_key,upload_id,size_bytes,content_type,status,
        created_at,upload_expires_at,ready_at,folder_id)
      VALUES(?,?,?,?,?,?,?,?,?,?,?,?)`).run(id,owner.user_id,name,key,`upload_${id}`,
        bytes.length,contentType,status,TIME,TIME+60000,TIME,folder);
    objects.set(key,bytes);
    return id;
  }
  const row = id => db.prepare('SELECT * FROM cloud_drive_videos WHERE id=?').get(id);
  const folder = async (name,principal = owner) => {
    const created = await call('POST','/v1/cloud-drive/folders',{ name },principal);
    assert.equal(created.statusCode,201);
    return created.body.id;
  };
  return { db,owner,other,objects,readKeys,deletedKeys,service,call,media,row,folder };
}

test('renaming preserves object identity, signed reads, content, quota and later deletion', async t => {
  const f = fixture(t);
  const folder = await f.folder('Clips');
  const id = f.media({ name:'iPhone clip.MOV',contentType:'video/quicktime',folder });
  const before = f.row(id);
  const digest = createHash('sha256').update(f.objects.get(before.object_key)).digest('hex');
  const originalLink = await f.call('GET',`${VIDEOS}/${id}/play-link`);
  const quota = (await f.call('GET',VIDEOS)).body.quota;
  const renamed = await f.call('PATCH',`${VIDEOS}/${id}`,{ name:'旅行 🌍.mov' });
  assert.equal(renamed.statusCode,200);
  assert.deepEqual(renamed.body,{ video_id:id,status:'ready',name:'旅行 🌍.mov',
    size_bytes:before.size_bytes,content_type:'video/quicktime',media_type:'video',
    folder_id:folder,created_at:TIME });
  assert.deepEqual(f.row(id),{ ...before,name:'旅行 🌍.mov' });
  const listed = await f.call('GET',VIDEOS);
  assert.equal(listed.body.videos[0].name,'旅行 🌍.mov');
  assert.deepEqual(listed.body.quota,quota);
  const signed = await f.call('GET',`${VIDEOS}/${id}/play-link`);
  assert.equal(signed.statusCode,200);
  assert.equal(signed.body.read_url,originalLink.body.read_url);
  assert.deepEqual(f.readKeys,[before.object_key,before.object_key]);
  // Resolve the issued object URL as an OSS reader would. Metadata renaming
  // does not create a second object or change any downloadable source bytes.
  const readKey = decodeURIComponent(new URL(signed.body.read_url).pathname.slice(1));
  assert.equal(createHash('sha256').update(f.objects.get(readKey)).digest('hex'),digest);
  assert.equal(f.objects.size,1);
  assert.equal((await f.call('GET',`${VIDEOS}/${id}/thumbnail-link`)).statusCode,200);
  assert.equal((await f.call('DELETE',`${VIDEOS}/${id}`)).statusCode,200);
  assert.deepEqual(f.deletedKeys,[before.object_key]);
  assert.equal(f.objects.size,0);
  assert.equal(f.row(id),undefined);
  assert.equal((await f.call('GET',`${VIDEOS}/${id}/play-link`)).statusCode,404);
  assert.equal((await f.call('GET',VIDEOS)).body.quota.used_bytes,0);
});

test('rename and folder move form one atomic metadata update', async t => {
  const f = fixture(t);
  const source = await f.folder('Source');
  const target = await f.folder('Target');
  const foreign = await f.folder('Other account',f.other);
  const id = f.media({ folder:source });
  const before = f.row(id);
  for (const [body,code] of [
    [{ name:'renamed.mp4',folder_id:foreign },404],
    [{ name:'renamed.mp4',folder_id:randomUUID() },404],
    [{ name:'renamed.mov',folder_id:target },400],
    [{ name:'../renamed.mp4',folder_id:target },400],
    [{ name:'renamed.mp4',folder_id:null },400],
    [{ name:'renamed.mp4',folder_id:target,recursive:true },400]
  ]) {
    assert.equal((await f.call('PATCH',`${VIDEOS}/${id}`,body)).statusCode,code);
    assert.deepEqual(f.row(id),before);
  }
  const renamed = await f.call('PATCH',`${VIDEOS}/${id}`,
    { name:'renamed.MP4',folder_id:target });
  assert.equal(renamed.statusCode,200);
  assert.deepEqual(f.row(id),{ ...before,name:'renamed.MP4',folder_id:target });
  assert.equal((await f.call('PATCH',`${VIDEOS}/${id}`,{ folder_id:'' })).statusCode,200);
  assert.deepEqual(f.row(id),{ ...before,name:'renamed.MP4',folder_id:null });
});

test('rename rejects malformed updates, unsafe names and ill-formed Unicode without mutations', async t => {
  const f = fixture(t);
  const id = f.media();
  const before = f.row(id);
  for (const body of [{},null,[],42,'name.mp4',{ name:3 },{ name:null },
    { name:true },{ folder_id:3 },{ name:'valid.mp4',object_key:'replace' }]) {
    assert.equal((await f.call('PATCH',`${VIDEOS}/${id}`,body)).statusCode,400);
    assert.deepEqual(f.row(id),before);
  }
  for (const name of ['', '.', '..', '.mp4', ' leading.mp4', 'trailing.mp4 ',
    'nested/file.mp4', 'nested\\file.mp4', 'bad:name.mp4', 'question?.mp4',
    'CON.mp4', 'aux.MP4', 'LPT9.clip.mp4', 'line\n.mp4', 'nul\0.mp4',
    'control\u0085.mp4', 'line\u2028.mp4', 'line\u2029.mp4', '\u00a0leading.mp4',
    'trailing.mp4\u3000', '\ufeffleading.mp4', '\ud800bad.mp4', 'bad\udfff.mp4',
    'a'.repeat(237)+'.mp4', '🌍'.repeat(60)+'.mp4']) {
    assert.equal((await f.call('PATCH',`${VIDEOS}/${id}`,{ name })).statusCode,400,
      JSON.stringify(name));
    assert.deepEqual(f.row(id),before);
  }
  const boundary = '🌍'.repeat(59)+'.mp4';
  assert.equal(Buffer.byteLength(boundary,'utf8'),240);
  assert.equal((await f.call('PATCH',`${VIDEOS}/${id}`,{ name:boundary })).statusCode,200);
  const decomposed = 'e\u0301'.repeat(79)+'.mp4';
  assert.ok(Buffer.byteLength(decomposed,'utf8') > 240);
  const normalized = await f.call('PATCH',`${VIDEOS}/${id}`,{ name:decomposed });
  assert.equal(normalized.statusCode,200);
  assert.equal(normalized.body.name,decomposed.normalize('NFC'));
});

test('rename preserves exact format extensions while permitting letter case changes', async t => {
  const f = fixture(t);
  for (const [name,contentType,valid,invalid] of [
    ['old.mp4','video/mp4','new.MP4','new.mov'],
    ['old.MOV','video/quicktime','new.mov','new.mp4'],
    ['old.jpeg','image/jpeg','new.JPEG','new.jpg'],
    ['old.jpg','image/jpeg','new.JPG','new.jpeg'],
    ['old.PNG','image/png','new.png','new.webp'],
    ['old.webp','image/webp','new.WEBP','new.png'],
    ['old.HEIC','image/heic','new.heic','new.jpg']
  ]) {
    const id = f.media({ name,contentType });
    const before = f.row(id);
    assert.equal((await f.call('PATCH',`${VIDEOS}/${id}`,{ name:invalid })).statusCode,400);
    assert.deepEqual(f.row(id),before);
    assert.equal((await f.call('PATCH',`${VIDEOS}/${id}`,{ name:valid })).statusCode,200);
    assert.deepEqual(f.row(id),{ ...before,name:valid });
  }
});

test('rename requires the owner and ready state and never mutates Session attachments', async t => {
  const f = fixture(t);
  const id = f.media();
  assert.equal((await f.call('PATCH',`${VIDEOS}/${id}`,{ name:'foreign.mp4' },f.other))
    .statusCode,404);
  assert.equal(f.row(id).name,'original.mp4');
  for (const status of ['uploading','completing','deleted']) {
    const unavailable = f.media({ status });
    const before = f.row(unavailable);
    assert.equal((await f.call('PATCH',`${VIDEOS}/${unavailable}`,{ name:'new.mp4' }))
      .statusCode,404);
    assert.deepEqual(f.row(unavailable),before);
  }
  const session = createCloudDriveService({ db:f.db,mode:'session' });
  assert.equal((await session.handle('PATCH',`/v1/session-attachments/${id}`,
    { name:'session.mp4' },f.owner)).statusCode,404);
  assert.equal((await f.call('PATCH',`${VIDEOS}/${randomUUID()}`,{ name:'new.mp4' }))
    .statusCode,404);
});
