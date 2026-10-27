'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const { openStore } = require('./store');
const { createCloudDriveService } = require('./cloud-drive-service');

test('session attachments use a separate OSS prefix, quota and room binding', async t => {
  const db = openStore(':memory:');
  t.after(() => db.close());
  const now = Date.parse('2026-10-27T04:00:00Z');
  const owner = { user_id:'user-a',account_id:'account_a' };
  const other = { user_id:'user-b',account_id:'account_b' };
  for (const user of [owner,other]) db.prepare(`INSERT INTO users
    (id,account_id,im_user_id,created_at,updated_at) VALUES(?,?,?,?,?)`)
    .run(user.user_id,user.account_id,user.account_id,now,now);
  const keys = [];
  const service = createCloudDriveService({ db,mode:'session',now:() => now,
    startUpload:async (_account,key) => { keys.push(key); return 'upload_a'; },
    abortUpload:async () => {},
    signRead:async (_account,key) => `https://oss.example/${key}?signed=1` });
  const upload = (name,type,room = 'session_a') => service.handle('POST',
    '/v1/session-attachments/uploads',{
      name,size_bytes:1024,content_type:type,owner_slot:'mac',
      owner_login_session_id:'login_a',session_id:room },owner);
  assert.equal((await upload('voice.m4a','audio/mp4','bad/room')).statusCode,400);
  assert.equal((await upload('CON.pdf','application/octet-stream')).statusCode,400);
  assert.equal((await upload('bad?.jpg','image/jpeg')).statusCode,400);
  const audio = await upload('voice.m4a','audio/mp4');
  const file = await upload('report.pdf','application/octet-stream');
  assert.equal(audio.statusCode,201);
  assert.equal(file.statusCode,201);
  assert.equal(audio.body.room_key,'mac:session_a');
  assert.match(keys[0],/^session-attachments\/account_a\//);
  assert.equal(db.prepare('SELECT COUNT(*) AS count FROM cloud_drive_videos').get().count,0);
  assert.equal(db.prepare('SELECT COUNT(*) AS count FROM session_attachments').get().count,2);
  db.prepare(`INSERT INTO cloud_drive_videos(id,owner_user_id,name,object_key,
    size_bytes,content_type,status,created_at,upload_expires_at,ready_at)
    VALUES(?,?,?,?,?,?,'ready',?,?,?)`).run(
      '123e4567-e89b-42d3-a456-426614174099',owner.user_id,'personal.jpg',
      'cloud-drive/account_a/2026-10-27/123e4567-e89b-42d3-a456-426614174099.jpg',
      500,'image/jpeg',now,now+10000,now);
  db.prepare(`UPDATE session_attachments SET status='ready' WHERE id=?`)
    .run(audio.body.attachment_id);
  const list = await service.handle('GET','/v1/session-attachments',{},owner);
  assert.equal(list.body.quota.used_bytes,1024);
  assert.equal(list.body.quota.reserved_bytes,1024);
  const link = await service.handle('GET',
    `/v1/session-attachments/${audio.body.attachment_id}/play-link`,{},owner);
  assert.equal(link.statusCode,200);
  assert.equal(link.body.media_type,'audio');
  assert.equal(link.body.room_key,'mac:session_a');
  assert.equal((await service.handle('GET',
    `/v1/session-attachments/${audio.body.attachment_id}/play-link`,{},other))
    .statusCode,404);
  const removed = await service.handle('POST','/v1/session-attachments/cleanup-room',{
    owner_slot:'mac',owner_login_session_id:'login_a',session_id:'session_a' },owner);
  assert.equal(removed.statusCode,202);
  assert.equal(removed.body.deleted,2);
  assert.equal((await service.handle('GET',
    `/v1/session-attachments/${audio.body.attachment_id}/play-link`,{},owner))
    .statusCode,404);
  const replacedLogin = await service.handle('POST',
    '/v1/session-attachments/uploads',{
      name:'again.pdf',size_bytes:100,content_type:'application/octet-stream',
      owner_slot:'mac',owner_login_session_id:'login_b',session_id:'session_a'
    },owner);
  assert.equal(replacedLogin.statusCode,201);
  assert.equal(replacedLogin.body.room_key,'mac:session_a');
});
