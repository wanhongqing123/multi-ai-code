'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const { openStore } = require('./store');
const { createAuthService } = require('./service');
const { createServer:createAuthServer,cloudDriveFromEnv,
  cloudDriveAllowedAccountsFromEnv } = require('./server');
const { createHandler:createMediaHandler } = require('../oss-media-signer/index');
const { createServer:createMediaServer } = require('../oss-media-signer/server');

test('cloud drive stays off unless explicitly enabled with signer and quota', () => {
  assert.equal(cloudDriveFromEnv({}),null);
  assert.equal(cloudDriveFromEnv({ MAICHAT_CLOUD_DRIVE_ENABLED:'0' }),null);
  assert.throws(() => cloudDriveFromEnv({ MAICHAT_CLOUD_DRIVE_ENABLED:'1' }),
    /signer or quota/);
  assert.throws(() => cloudDriveFromEnv({ MAICHAT_CLOUD_DRIVE_ENABLED:'1',
    MAICHAT_CLOUD_DRIVE_INTERNAL_KEY:'a'.repeat(32),
    MAICHAT_CLOUD_DRIVE_QUOTA_BYTES:'1999999999' }),/signer or quota/);
});

test('cloud drive account allowlist accepts only complete valid account IDs', () => {
  assert.equal(cloudDriveAllowedAccountsFromEnv(undefined),null);
  assert.equal(cloudDriveAllowedAccountsFromEnv('*'),null);
  assert.deepEqual([...cloudDriveAllowedAccountsFromEnv('cloud_owner, cloud_viewer')],
    ['cloud_owner','cloud_viewer']);
  for (const value of ['', ' ', '* , cloud_owner', 'cloud_owner,',
    'cloud_owner,,cloud_viewer', 'cloud_owner,cloud_owner', 'Administrator',
    'administrator', 'invalid.account', 123])
    assert.throws(() => cloudDriveAllowedAccountsFromEnv(value),
      /MAICHAT_CLOUD_DRIVE_ALLOWED_ACCOUNTS/);
  assert.throws(() => cloudDriveFromEnv({ MAICHAT_CLOUD_DRIVE_ENABLED:'1',
    MAICHAT_CLOUD_DRIVE_INTERNAL_KEY:'cloud-test-internal-key-at-least-32-characters',
    MAICHAT_CLOUD_DRIVE_ALLOWED_ACCOUNTS:'cloud_owner,' }),
    /MAICHAT_CLOUD_DRIVE_ALLOWED_ACCOUNTS/);
});

test('cloud drive canary permits the logged-in account and hides all routes from peers',
  async () => {
    const db = openStore(':memory:');
    let signerCalls = 0;
    const cloudDrive = cloudDriveFromEnv({ MAICHAT_CLOUD_DRIVE_ENABLED:'1',
      MAICHAT_CLOUD_DRIVE_INTERNAL_KEY:'cloud-test-internal-key-at-least-32-characters',
      MAICHAT_CLOUD_DRIVE_ALLOWED_ACCOUNTS:'cloud_owner' },async () => {
      signerCalls++;
      return { status:200,json:async () => ({ upload_id:'upload_canary_1' }) };
    });
    const service = createAuthService({ db,sms:{ async send() {} },
      secret:'local-auth-hmac-key-at-least-32-characters',
      imSecret:'local-im-secret-key-at-least-32-characters',sdkAppId:123456789,
      cloudDrive });
    const server = createAuthServer(service);
    await new Promise(resolve => server.listen(0,'127.0.0.1',resolve));
    const base = `http://127.0.0.1:${server.address().port}`;
    const call = (path,token,method = 'GET',body) => fetch(`${base}${path}`,{
      method,headers:{ Authorization:`Bearer ${token}`,
        ...(body ? { 'Content-Type':'application/json' } : {}) },
      ...(body ? { body:JSON.stringify(body) } : {}) });
    try {
      for (const id of ['cloud_owner','cloud_viewer'])
        assert.equal((await service.createTestAccount(id,`${id}-password-123456`))
          .statusCode,201);
      async function login(id) {
        const response = await fetch(`${base}/v1/auth/password/login`,{
          method:'POST',headers:{ 'Content-Type':'application/json' },
          body:JSON.stringify({ account_id:id,password:`${id}-password-123456`,
            device_slot:'ios_phone',age_confirmed_14_plus:true }) });
        assert.equal(response.status,200);
        return (await response.json()).access_token;
      }
      const owner = await login('cloud_owner');
      const viewer = await login('cloud_viewer');
      assert.equal((await call('/v1/cloud-drive/folders',viewer)).status,404);
      assert.equal((await call('/v1/cloud-drive/folders',viewer,'POST',
        { name:'Hidden' })).status,404);
      assert.equal((await call('/v1/cloud-drive/videos',viewer)).status,404);
      assert.equal((await call('/v1/cloud-drive/videos/uploads',viewer,'POST',
        { name:'private.mp4',size_bytes:1000,content_type:'video/mp4' })).status,404);
      assert.equal((await call('/v1/cloud-drive/videos/unknown/play-link',viewer)).status,404);
      assert.equal(signerCalls,0);
      assert.equal(db.prepare('SELECT COUNT(*) AS count FROM cloud_drive_videos')
        .get().count,0);
      assert.equal((await call('/v1/cloud-drive/videos',owner)).status,200);
      assert.equal((await call('/v1/cloud-drive/videos/uploads',owner,'POST',
        { name:'private.mp4',size_bytes:1000,content_type:'video/mp4' })).status,201);
      assert.equal(signerCalls,1);
    } finally {
      await new Promise(resolve => server.close(resolve));
      db.close();
    }
  });

test('authenticated two-account private media upload uses OSS multipart control', async () => {
  const db = openStore(':memory:');
  const secret = 'cloud-test-internal-key-at-least-32-characters';
  const objects = new Map();
  const uploads = new Map();
  let authService;
  const mediaHandler = createMediaHandler({
    env:{ MAICHAT_CLOUD_DRIVE_ENABLED:'1',MAICHAT_CLOUD_DRIVE_INTERNAL_KEY:secret,
      MAICHAT_OSS_BUCKET:'private-test-bucket',
      MAICHAT_OSS_REGION:'oss-cn-hangzhou',
      ALIBABA_CLOUD_ACCESS_KEY_ID:'local-key-id',
      ALIBABA_CLOUD_ACCESS_KEY_SECRET:'local-key-secret',
      MAICHAT_CLOUD_DRIVE_OSS_BUCKET:'private-cloud-test-bucket',
      MAICHAT_CLOUD_DRIVE_OSS_REGION:'oss-cn-hangzhou',
      MAICHAT_CLOUD_DRIVE_OSS_ACCESS_KEY_ID:'local-cloud-key-id',
      MAICHAT_CLOUD_DRIVE_OSS_ACCESS_KEY_SECRET:'local-cloud-key-secret' },
    makeClient:() => ({
      initMultipartUpload:async (key,options) => {
        objects.set(key,{ parts:[],type:options.mime,size:null });
        uploads.set('fakeUpload_123',key);
        return { uploadId:'fakeUpload_123' };
      },
      listUploads:async query => ({ uploads:[...uploads]
        .filter(([,key]) => key.startsWith(query.prefix))
        .map(([uploadId,key]) => ({ name:key,uploadId })),
      isTruncated:false }),
      signatureUrlV4:async (method,_ttl,options,key) =>
        `https://private-test-bucket.oss-cn-hangzhou.aliyuncs.com/${key}` +
        `?signed=${method}&partNumber=${options?.queries?.partNumber || 0}`,
      listParts:async key => ({ parts:objects.get(key).parts.map(part => ({
        PartNumber:part.part_number,Size:part.size_bytes,ETag:part.etag })) }),
      completeMultipartUpload:async (key,uploadId) => {
        const object = objects.get(key);
        object.size = object.parts.reduce((sum,part) => sum+part.size_bytes,0);
        uploads.delete(uploadId);
      },
      abortMultipartUpload:async (_key,uploadId) => { uploads.delete(uploadId); },
      head:async key => {
        const object = objects.get(key);
        if (!object || object.size === null) {
          const missing = new Error('NoSuchKey');
          missing.status = 404;
          throw missing;
        }
        return { res:{ headers:{ 'content-length':String(object.size),
          'content-type':object.type } } };
      },
      delete:async key => { objects.delete(key); }
    })
  });
  const mediaServer = createMediaServer(mediaHandler);
  await new Promise(resolve => mediaServer.listen(0,'127.0.0.1',resolve));
  const cloudOptions = cloudDriveFromEnv({ MAICHAT_CLOUD_DRIVE_ENABLED:'1',
    MAICHAT_CLOUD_DRIVE_INTERNAL_KEY:secret,
    MAICHAT_MEDIA_PORT:String(mediaServer.address().port) });
  authService = createAuthService({ db,sms:{ async send() {} },
    secret:'local-auth-hmac-key-at-least-32-characters',
    imSecret:'local-im-secret-key-at-least-32-characters',sdkAppId:123456789,
    cloudDrive:cloudOptions,sessionAttachments:cloudOptions });
  const authServer = createAuthServer(authService);
  await new Promise(resolve => authServer.listen(0,'127.0.0.1',resolve));
  const base = `http://127.0.0.1:${authServer.address().port}`;
  const post = (path,token,body = {}) => fetch(`${base}${path}`,{
    method:'POST',headers:{ Authorization:`Bearer ${token}`,
      'Content-Type':'application/json' },body:JSON.stringify(body) });
  try {
    for (const id of ['cloud_owner','cloud_viewer'])
      assert.equal((await authService.createTestAccount(id,`${id}-password-123456`))
        .statusCode,201);
    async function login(id) {
      const response = await fetch(`${base}/v1/auth/password/login`,{
        method:'POST',headers:{ 'Content-Type':'application/json' },
        body:JSON.stringify({ account_id:id,password:`${id}-password-123456`,
          device_slot:'ios_phone',age_confirmed_14_plus:true })
      });
      assert.equal(response.status,200);
      return (await response.json()).access_token;
    }
    const owner = await login('cloud_owner');
    const viewer = await login('cloud_viewer');
    assert.equal((await fetch(`${base}/v1/cloud-drive/videos`)).status,401);
    const created = await post('/v1/cloud-drive/videos/uploads',owner,
      { name:'iPhone clip.mov',size_bytes:1000,content_type:'video/quicktime' });
    assert.equal(created.status,201);
    const { video_id:id } = await created.json();
    const key = [...objects.keys()][0];
    assert.match(key,/^cloud-drive\/cloud_owner\/\d{4}-\d{2}-\d{2}\//);
    assert.equal((await fetch(`${base}/v1/cloud-drive/videos/uploads/${id}/parts`,{
      headers:{ Authorization:`Bearer ${viewer}` } })).status,404);
    const md5 = Buffer.alloc(16,5).toString('base64');
    const signed = await post(`/v1/cloud-drive/videos/uploads/${id}/parts/1/sign`,
      owner,{ content_md5_base64:md5 });
    assert.equal(signed.status,200);
    const part = await signed.json();
    assert.equal(part.part_size_bytes,1000);
    assert.equal(part.upload_headers['Content-Length'],'1000');
    objects.get(key).parts.push({ part_number:1,size_bytes:1000,
      etag:'"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"' });
    const list = await fetch(`${base}/v1/cloud-drive/videos/uploads/${id}/parts`,{
      headers:{ Authorization:`Bearer ${owner}` } });
    assert.equal((await list.json()).parts.length,1);
    assert.equal((await post(`/v1/cloud-drive/videos/uploads/${id}/complete`,owner))
      .status,200);
    const ownerList = await fetch(`${base}/v1/cloud-drive/videos`,{
      headers:{ Authorization:`Bearer ${owner}` } });
    const videos = await ownerList.json();
    assert.equal(videos.videos[0].id,id);
    assert.deepEqual(videos.quota,{ used_bytes:1000,reserved_bytes:0,
      limit_bytes:10_000_000_000 });
    const folderRequest = (route,method = 'GET',body,token = owner) =>
      fetch(`${base}${route}`,{ method,headers:{ Authorization:`Bearer ${token}`,
        ...(body ? { 'Content-Type':'application/json' } : {}) },
        ...(body ? { body:JSON.stringify(body) } : {}) });
    const renamed = await folderRequest(`/v1/cloud-drive/videos/${id}`,
      'PATCH',{ name:'Holiday.MOV' });
    assert.equal(renamed.status,200);
    const renamedBody = await renamed.json();
    assert.equal(renamedBody.video_id,id);
    assert.equal(renamedBody.name,'Holiday.MOV');
    assert.equal(renamedBody.content_type,'video/quicktime');
    assert.equal((await folderRequest(`/v1/cloud-drive/videos/${id}`,
      'PATCH',{ name:'Wrong.mp4' })).status,400);
    assert.equal((await folderRequest(`/v1/cloud-drive/videos/${id}`,
      'PATCH',{ name:'\ud800Invalid.mov' })).status,400);
    assert.equal((await folderRequest(`/v1/cloud-drive/videos/${id}`,
      'PATCH',{ name:'Foreign.mov' },viewer)).status,404);
    const afterRename = await folderRequest('/v1/cloud-drive/videos');
    const afterRenameBody = await afterRename.json();
    assert.equal(afterRenameBody.videos[0].name,'Holiday.MOV');
    assert.deepEqual(afterRenameBody.quota,videos.quota);
    assert.equal((await fetch(`${base}/v1/cloud-drive/folders`)).status,401);
    const folderCreated = await folderRequest('/v1/cloud-drive/folders','POST',
      { name:'旅行' });
    assert.equal(folderCreated.status,201);
    const folder = await folderCreated.json();
    assert.equal(folder.name,'旅行');
    assert.equal(folder.parent_id,null);
    const rename = await folderRequest(`/v1/cloud-drive/folders/${folder.id}`,
      'PATCH',{ name:'Holiday' });
    assert.equal(rename.status,200);
    assert.equal((await rename.json()).name,'Holiday');
    const moved = await folderRequest(`/v1/cloud-drive/videos/${id}`,
      'PATCH',{ folder_id:folder.id });
    assert.equal(moved.status,200);
    assert.equal((await moved.json()).folder_id,folder.id);
    const filtered = await folderRequest(`/v1/cloud-drive/videos?folder_id=${folder.id}`);
    assert.equal((await filtered.json()).videos[0].id,id);
    const rootOnly = await folderRequest('/v1/cloud-drive/videos?folder_id=');
    assert.deepEqual((await rootOnly.json()).videos,[]);
    assert.deepEqual((await (await folderRequest('/v1/cloud-drive/folders','GET',
      undefined,viewer)).json()).folders,[]);
    assert.equal((await folderRequest(`/v1/cloud-drive/folders/${folder.id}`,
      'DELETE',undefined,viewer)).status,404);
    assert.equal((await folderRequest(`/v1/cloud-drive/folders/${folder.id}`,
      'DELETE')).status,409);
    assert.equal((await folderRequest(`/v1/cloud-drive/videos/${id}`,
      'PATCH',{ folder_id:'' })).status,200);
    const folderDeleted = await folderRequest(`/v1/cloud-drive/folders/${folder.id}`,
      'DELETE');
    assert.equal(folderDeleted.status,200);
    assert.deepEqual(await folderDeleted.json(),{ folder_id:folder.id,status:'deleted' });
    assert.equal(objects.get(key).size,1000);
    assert.equal((await fetch(`${base}/v1/cloud-drive/videos/${id}/play-link`,{
      headers:{ Authorization:`Bearer ${viewer}` } })).status,404);
    const played = await fetch(`${base}/v1/cloud-drive/videos/${id}/play-link`,{
      headers:{ Authorization:`Bearer ${owner}` } });
    assert.equal(played.status,200);
    assert.equal((await played.json()).read_expires_in,60);
    assert.equal((await fetch(`${base}/v1/cloud-drive/videos/${id}`,{
      method:'DELETE',headers:{ Authorization:`Bearer ${owner}` } })).status,200);
    assert.equal(objects.has(key),false);
    const image = await post('/v1/cloud-drive/videos/uploads',owner,
      { name:'iPhone photo.heic',size_bytes:1200,content_type:'image/heic' });
    assert.equal(image.status,201);
    const imageBody = await image.json();
    assert.equal(imageBody.media_type,'image');
    const imageKey = [...objects.keys()][0];
    assert.ok(imageKey.endsWith('.heic'));
    objects.get(imageKey).parts.push({ part_number:1,size_bytes:1200,
      etag:'"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"' });
    const imageComplete = await post(
      `/v1/cloud-drive/videos/uploads/${imageBody.video_id}/complete`,owner);
    assert.equal(imageComplete.status,200);
    assert.equal((await imageComplete.json()).media_type,'image');
    assert.equal((await fetch(`${base}/v1/cloud-drive/videos/${imageBody.video_id}/thumbnail-link`,{
      headers:{ Authorization:`Bearer ${viewer}` } })).status,404);
    const preview = await fetch(
      `${base}/v1/cloud-drive/videos/${imageBody.video_id}/thumbnail-link`,{
        headers:{ Authorization:`Bearer ${owner}` } });
    assert.equal(preview.status,200);
    assert.equal((await preview.json()).thumbnail_expires_in,60);
    const mediaList = await fetch(`${base}/v1/cloud-drive/videos`,{
      headers:{ Authorization:`Bearer ${owner}` } });
    const items = await mediaList.json();
    assert.equal(items.videos.length,1);
    assert.equal(items.videos[0].media_type,'image');
    assert.equal(items.quota.used_bytes,1200);
    assert.equal((await fetch(`${base}/v1/cloud-drive/videos/${imageBody.video_id}`,{
      method:'DELETE',headers:{ Authorization:`Bearer ${owner}` } })).status,200);
    assert.equal(objects.has(imageKey),false);
    const room = { owner_slot:'mac',owner_login_session_id:'login_a',
      session_id:'session_a' };
    const attached = await post('/v1/session-attachments/uploads',owner,
      { ...room,name:'notes.pdf',size_bytes:1300,
        content_type:'application/octet-stream' });
    assert.equal(attached.status,201);
    const attachment = await attached.json();
    assert.equal(attachment.room_key,'mac:session_a');
    const attachmentKey = [...objects.keys()][0];
    assert.match(attachmentKey,/^session-attachments\/cloud_owner\//);
    objects.get(attachmentKey).parts.push({ part_number:1,size_bytes:1300,
      etag:'"cccccccccccccccccccccccccccccccc"' });
    assert.equal((await post(`/v1/session-attachments/uploads/${attachment.attachment_id}/complete`,
      owner)).status,200);
    const link = await fetch(
      `${base}/v1/session-attachments/${attachment.attachment_id}/play-link`,{
        headers:{ Authorization:`Bearer ${owner}` } });
    assert.equal(link.status,200);
    assert.equal((await link.json()).room_key,'mac:session_a');
    assert.equal((await fetch(
      `${base}/v1/session-attachments/${attachment.attachment_id}/play-link`,{
        headers:{ Authorization:`Bearer ${viewer}` } })).status,404);
    const separate = await fetch(`${base}/v1/cloud-drive/videos`,{
      headers:{ Authorization:`Bearer ${owner}` } });
    assert.equal((await separate.json()).videos.length,0);
    assert.equal((await post('/v1/session-attachments/cleanup-room',owner,room)).status,202);
    assert.equal((await fetch(
      `${base}/v1/session-attachments/${attachment.attachment_id}/play-link`,{
        headers:{ Authorization:`Bearer ${owner}` } })).status,404);
    await authService.cleanupSessionAttachments();
    assert.equal(objects.has(attachmentKey),false);
  } finally {
    await new Promise(resolve => authServer.close(resolve));
    await new Promise(resolve => mediaServer.close(resolve));
    db.close();
  }
});
