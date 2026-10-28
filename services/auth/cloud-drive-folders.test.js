'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const { randomUUID } = require('node:crypto');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { openStore } = require('./store');
const { createCloudDriveService } = require('./cloud-drive-service');
const { completeAccountDeletion,REQUIRED_EVIDENCE } = require('./account-deletion');

const FOLDERS = '/v1/cloud-drive/folders';
const VIDEOS = '/v1/cloud-drive/videos';
const TIME = Date.parse('2026-10-28T10:00:00Z');

function fixture(t) {
  const db = openStore(':memory:');
  t.after(() => db.close());
  const owner = { user_id:'folder-owner',account_id:'folder_owner' };
  const other = { user_id:'folder-other',account_id:'folder_other' };
  for (const user of [owner,other])
    db.prepare(`INSERT INTO users(id,account_id,im_user_id,created_at,updated_at)
      VALUES(?,?,?,?,?)`).run(user.user_id,user.account_id,user.account_id,TIME,TIME);
  let time = TIME;
  const service = createCloudDriveService({ db,now:() => time });
  const call = (method,route,body = {},principal = owner,query) =>
    service.handle(method,route,body,principal,
      query === undefined ? undefined : new URLSearchParams(query));
  const create = async (name,parent,principal = owner) => {
    const response = await call('POST',FOLDERS,
      { name,...(parent === undefined ? {} : { parent_id:parent }) },principal);
    assert.equal(response.statusCode,201,JSON.stringify(response));
    return response.body;
  };
  function media(status = 'ready',folder = null) {
    const id = randomUUID();
    db.prepare(`INSERT INTO cloud_drive_videos
      (id,owner_user_id,name,object_key,size_bytes,content_type,status,created_at,
      upload_expires_at,folder_id) VALUES(?,?,?,?,?,'video/mp4',?,?,?,?)`)
      .run(id,owner.user_id,'clip.mp4',`cloud-drive/folder_owner/${id}.mp4`,10,
        status,TIME,TIME+60000,folder);
    return id;
  }
  return { db,owner,other,service,call,create,media,advance:() => { time++; } };
}

test('folders support nested creation, rename, move, root and owner isolation', async t => {
  const f = fixture(t);
  assert.equal((await f.call('GET',FOLDERS,{},null)).statusCode,401);
  const root = await f.create('旅行');
  const child = await f.create('视频',root.id);
  const target = await f.create('工作','');
  assert.deepEqual(root,{ id:root.id,name:'旅行',parent_id:null,
    created_at:TIME,updated_at:TIME });
  assert.equal((await f.call('GET',FOLDERS,{},f.other)).body.folders.length,0);
  for (const method of ['PATCH','DELETE'])
    assert.equal((await f.call(method,`${FOLDERS}/${root.id}`,
      { name:'Stolen' },f.other)).statusCode,404);
  assert.equal((await f.call('POST',FOLDERS,
    { name:'foreign',parent_id:root.id },f.other)).statusCode,404);
  const foreign = await f.create('other',undefined,f.other);
  assert.equal((await f.call('PATCH',`${FOLDERS}/${child.id}`,
    { parent_id:foreign.id })).statusCode,404);
  f.advance();
  const moved = await f.call('PATCH',`${FOLDERS}/${child.id}`,
    { name:'已剪辑',parent_id:target.id });
  assert.deepEqual(moved.body,{ ...child,name:'已剪辑',parent_id:target.id,
    updated_at:TIME+1 });
  const toRoot = await f.call('PATCH',`${FOLDERS}/${child.id}`,{ parent_id:'' });
  assert.equal(toRoot.body.parent_id,null);
  assert.equal((await f.call('GET',FOLDERS)).body.folders.length,3);
  assert.equal((await f.call('DELETE',`${FOLDERS}/${target.id}`)).statusCode,200);
  assert.equal((await f.call('DELETE',`${FOLDERS}/${target.id}`)).statusCode,404);
});

test('folder validation rejects unsafe names, duplicate siblings and recursive moves atomically', async t => {
  const f = fixture(t);
  const root = await f.create('same');
  const child = await f.create('child',root.id);
  const leaf = await f.create('leaf',child.id);
  await f.create('same',child.id);
  await f.create('same',undefined,f.other);
  for (const name of ['', ' ', ' before', 'after ', '.', '..', 'a/b', 'a\\b',
    'a\n', 'CON', 'LPT1.txt', 'a.', 'a:b', 'x'.repeat(241),'\u0344'.repeat(120),null,123])
    assert.equal((await f.call('POST',FOLDERS,{ name })).statusCode,400,String(name));
  assert.equal((await f.call('POST',FOLDERS,{ name:'same' })).statusCode,409);
  await f.create('é');
  assert.equal((await f.call('POST',FOLDERS,{ name:'e\u0301' })).statusCode,409);
  assert.equal((await f.call('PATCH',`${FOLDERS}/${root.id}`,{})).statusCode,400);
  assert.equal((await f.call('POST',FOLDERS,{ name:'bad',parent_id:null })).statusCode,400);
  for (const parent_id of [root.id,leaf.id])
    assert.equal((await f.call('PATCH',`${FOLDERS}/${root.id}`,
      { name:'should not rename',parent_id })).statusCode,409);
  assert.equal((await f.call('PATCH',`${FOLDERS}/${root.id}`,
    { parent_id:'invalid' })).statusCode,400);
  assert.equal((await f.call('PATCH',`${FOLDERS}/${root.id}`,
    { parent_id:randomUUID() })).statusCode,404);
  assert.equal((await f.call('PATCH',`${FOLDERS}/${leaf.id}`,
    { name:'same' })).statusCode,409);
  const after = (await f.call('GET',FOLDERS)).body.folders.find(row => row.id === root.id);
  assert.equal(after.name,'same');
  assert.equal(after.parent_id,null);
  assert.equal((await f.call('DELETE',`${FOLDERS}/${root.id}`)).statusCode,409);
  assert.equal(f.db.pragma('foreign_key_check').length,0);
});

test('ready file moves and folder filters preserve storage keys, quota and unfiltered listing', async t => {
  const f = fixture(t);
  const folder = await f.create('Clips');
  const foreign = await f.create('Foreign',undefined,f.other);
  const id = f.media();
  const rootID = f.media();
  const uploading = f.media('uploading');
  const before = f.db.prepare('SELECT * FROM cloud_drive_videos WHERE id=?').get(id);
  const quota = (await f.call('GET',VIDEOS)).body.quota;
  const moved = await f.call('PATCH',`${VIDEOS}/${id}`,{ folder_id:folder.id });
  assert.equal(moved.statusCode,200);
  assert.equal(moved.body.folder_id,folder.id);
  const after = f.db.prepare('SELECT * FROM cloud_drive_videos WHERE id=?').get(id);
  assert.deepEqual(after,{ ...before,folder_id:folder.id });
  const listing = query => f.call('GET',VIDEOS,{},f.owner,query);
  assert.equal((await listing()).body.videos.length,2);
  assert.equal((await listing()).body.videos.find(row => row.id === id).folder_id,folder.id);
  assert.deepEqual((await listing({ folder_id:folder.id })).body.videos.map(row => row.id),[id]);
  assert.deepEqual((await listing({ folder_id:'' })).body.videos.map(row => row.id),[rootID]);
  assert.deepEqual((await listing()).body.quota,quota);
  assert.equal((await listing({ folder_id:foreign.id })).statusCode,404);
  assert.equal((await listing({ folder_id:'invalid' })).statusCode,400);
  assert.equal((await f.call('PATCH',`${VIDEOS}/${id}`,{ folder_id:foreign.id })).statusCode,404);
  assert.equal((await f.call('PATCH',`${VIDEOS}/${id}`,
    { folder_id:folder.id },f.other)).statusCode,404);
  assert.equal((await f.call('PATCH',`${VIDEOS}/${uploading}`,{ folder_id:folder.id })).statusCode,404);
  assert.equal((await f.call('PATCH',`${VIDEOS}/${id}`,{ folder_id:null })).statusCode,400);
  assert.equal((await f.call('DELETE',`${FOLDERS}/${folder.id}`)).statusCode,409);
  assert.equal((await f.call('PATCH',`${VIDEOS}/${id}`,{ folder_id:'' })).body.folder_id,null);
  assert.equal((await f.call('DELETE',`${FOLDERS}/${folder.id}`)).statusCode,200);
});

test('folder deletion rejects unfinished media and permits tombstones without changing cleanup state', async t => {
  const f = fixture(t);
  const folder = await f.create('Pending');
  const id = f.media('uploading',folder.id);
  assert.equal((await f.call('DELETE',`${FOLDERS}/${folder.id}`)).statusCode,409);
  f.db.prepare(`UPDATE cloud_drive_videos SET status='deleted',cleanup_pending=1,
    cleanup_revision=7 WHERE id=?`).run(id);
  const before = f.db.prepare('SELECT * FROM cloud_drive_videos WHERE id=?').get(id);
  assert.equal((await f.call('DELETE',`${FOLDERS}/${folder.id}`)).statusCode,200);
  assert.deepEqual(f.db.prepare('SELECT * FROM cloud_drive_videos WHERE id=?').get(id),
    { ...before,folder_id:null });
});

test('folder filtering composes with existing cursors without skipping same-timestamp files', async t => {
  const f = fixture(t);
  const folder = await f.create('Paged');
  const expected = [];
  for (let i=0;i<55;i++) expected.push(f.media('ready',folder.id));
  for (let i=0;i<5;i++) f.media();
  const first = await f.call('GET',VIDEOS,{},f.owner,{ folder_id:folder.id });
  assert.equal(first.body.videos.length,50);
  assert.ok(first.body.next_cursor);
  const second = await f.call('GET',VIDEOS,{},f.owner,
    { folder_id:folder.id,before:first.body.next_cursor });
  assert.equal(second.body.videos.length,5);
  assert.equal(second.body.next_cursor,null);
  assert.deepEqual([...first.body.videos,...second.body.videos].map(row => row.id).sort(),
    expected.sort());
  assert.equal((await f.call('GET',VIDEOS,{},f.owner,{ folder_id:'' })).body.videos.length,5);
});

test('folder metadata never exposes controls for the independent Session attachment space', async t => {
  const f = fixture(t);
  const session = createCloudDriveService({ db:f.db,mode:'session' });
  assert.equal((await session.handle('GET',FOLDERS,{},f.owner)).statusCode,404);
  assert.equal((await session.handle('POST',FOLDERS,{ name:'No' },f.owner)).statusCode,404);
  assert.equal((await session.handle('PATCH',`/v1/session-attachments/${randomUUID()}`,
    { folder_id:'' },f.owner)).statusCode,404);
  assert.equal((await session.handle('GET','/v1/session-attachments',{},f.owner,
    new URLSearchParams({ folder_id:'' }))).statusCode,400);
  assert.equal(f.db.pragma('table_info(session_attachments)').some(row => row.name === 'folder_id'),false);
});

test('account deletion clears nested folder metadata after required media cleanup', async t => {
  const f = fixture(t);
  const root = await f.create('root');
  await f.create('child',root.id);
  const other = await f.create('other',undefined,f.other);
  f.db.prepare('UPDATE users SET disabled_at=? WHERE id=?').run(TIME,f.owner.user_id);
  assert.equal((await f.call('POST',FOLDERS,{ name:'inactive account' })).statusCode,401);
  const request = randomUUID();
  f.db.prepare(`INSERT INTO account_deletion_requests
    (id,user_id,account_id,im_user_id,requested_at,due_at,status)
    VALUES(?,?,?,?,?,?,'pending')`).run(request,f.owner.user_id,
      f.owner.account_id,f.owner.account_id,TIME,TIME+100000);
  const evidence = { operator:'test_admin',records:{} };
  for (const name of REQUIRED_EVIDENCE) {
    evidence[name] = true;
    evidence.records[name] = `verified-${name}`;
  }
  assert.equal(completeAccountDeletion(f.db,request,evidence,
    'test-only-hmac-key-at-least-32-characters',TIME+1000).status,'completed');
  assert.deepEqual(f.db.prepare('SELECT id FROM cloud_drive_folders').all(),[{ id:other.id }]);
  assert.equal(f.db.pragma('foreign_key_check').length,0);
});

test('folder migration preserves an existing image schema and survives reopening', async t => {
  const directory = fs.mkdtempSync(path.join(os.tmpdir(),'cloud-folder-schema-'));
  t.after(() => fs.rmSync(directory,{ recursive:true,force:true }));
  const filename = path.join(directory,'auth.sqlite');
  const old = openStore(filename);
  old.prepare(`INSERT INTO users(id,account_id,im_user_id,created_at,updated_at)
    VALUES('owner','account_a','account_a',1,1)`).run();
  const id = randomUUID();
  old.prepare(`INSERT INTO cloud_drive_videos
    (id,owner_user_id,name,object_key,size_bytes,content_type,status,created_at,upload_expires_at)
    VALUES(?,'owner','image.png','cloud-drive/account_a/image.png',10,'image/png','ready',1,100)`)
    .run(id);
  // Recreate precisely the pre-folder image schema while retaining the data.
  old.exec('DROP INDEX cloud_drive_videos_folder');
  old.exec('ALTER TABLE cloud_drive_videos DROP COLUMN folder_id');
  old.exec('DROP TABLE cloud_drive_folders');
  old.close();
  const migrated = openStore(filename);
  assert.equal(migrated.prepare('SELECT folder_id FROM cloud_drive_videos WHERE id=?')
    .get(id).folder_id,null);
  const folder = randomUUID();
  migrated.prepare(`INSERT INTO cloud_drive_folders VALUES(?,'owner','Saved',NULL,1,1)`).run(folder);
  migrated.prepare('UPDATE cloud_drive_videos SET folder_id=? WHERE id=?').run(folder,id);
  migrated.close();
  const reopened = openStore(filename);
  t.after(() => reopened.close());
  assert.equal(reopened.prepare('SELECT folder_id FROM cloud_drive_videos WHERE id=?')
    .get(id).folder_id,folder);
  assert.equal(reopened.pragma('foreign_key_check').length,0);
});

test('folder limit applies to the owner and cannot be bypassed by nested creation', async t => {
  const f = fixture(t);
  f.db.transaction(() => {
    const insert = f.db.prepare(`INSERT INTO cloud_drive_folders VALUES(?,?,?,NULL,?,?)`);
    for (let i=0;i<1000;i++) insert.run(randomUUID(),f.owner.user_id,`folder-${i}`,TIME,TIME);
  })();
  const root = f.db.prepare('SELECT id FROM cloud_drive_folders LIMIT 1').get().id;
  assert.equal((await f.call('GET',FOLDERS)).body.folders.length,1000);
  assert.equal((await f.call('POST',FOLDERS,{ name:'over-limit',parent_id:root })).statusCode,429);
  await f.create('other-user',undefined,f.other);
});
