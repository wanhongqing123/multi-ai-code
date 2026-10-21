'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const { randomUUID } = require('node:crypto');
const { openStore } = require('./store');
const { createVideoCircleService } = require('./video-circle-service');
const { createAuthService } = require('./service');
const { createServer } = require('./server');

function fixture() {
  const db = openStore(':memory:');
  const add = db.prepare(`INSERT INTO users
    (id,account_id,phone_e164,im_user_id,created_at,updated_at)
    VALUES(?,?,NULL,?,?,?)`);
  add.run('author-id', 'author', 'author-im', 1, 1);
  add.run('viewer-id', 'viewer', 'viewer-im', 1, 1);
  let time = 1_800_000_000_000;
  const removed = [];
  const service = createVideoCircleService({
    db, now: () => time,
    verifyUpload: async (accountId, key, bytes) => ({
      exists: accountId === 'author' && key.startsWith('video-circle/author/'),
      size_bytes: bytes, content_type: 'video/mp4'
    }),
    signRead: async key => `https://private-oss.example/${key}?short-lived=yes`,
    deleteObject: async key => { removed.push(key); }
  });
  const owner = { user_id: 'author-id', account_id: 'author' };
  const viewer = { user_id: 'viewer-id', account_id: 'viewer' };
  const key = `video-circle/author/2026-10-22/${randomUUID()}.mp4`;
  const post = {
    title: '测试 AI 海边视频', object_key: key, source_tool: 'wan_video',
    source_task_id: 'task-123', size_bytes: 1000, duration_ms: 8000,
    ai_generated: true
  };
  return { db, service, owner, viewer, key, post, removed,
    advance: () => { time += 16 * 60_000; } };
}

test('video stays private until review; blocked viewers lose feed and playback', async () => {
  const f = fixture();
  const wrong = await f.service.handle('POST', '/v1/video-circle/posts',
    { ...f.post, object_key: f.key.replace('/author/', '/viewer/') }, f.owner);
  assert.equal(wrong.statusCode, 400);
  const created = await f.service.handle('POST', '/v1/video-circle/posts', f.post, f.owner);
  assert.equal(created.statusCode, 201);
  assert.equal(created.body.status, 'pending');
  const duplicate = await f.service.handle('POST', '/v1/video-circle/posts', f.post, f.owner);
  assert.equal(duplicate.body.post_id, created.body.post_id);
  const privateFeed = await f.service.handle('GET', '/v1/video-circle/feed', {}, f.viewer);
  assert.deepEqual(privateFeed.body.posts, []);
  const privatePlay = await f.service.handle('GET',
    `/v1/video-circle/posts/${created.body.post_id}/play`, {}, f.viewer);
  assert.equal(privatePlay.statusCode, 404);
  const ownerPreview = await f.service.handle('GET',
    `/v1/video-circle/posts/${created.body.post_id}/play`, {}, f.owner);
  assert.equal(ownerPreview.statusCode, 302);
  assert.equal(f.service.pendingReview().length, 1);

  assert.equal((await f.service.review(created.body.post_id, 'approve', 'admin'))
    .statusCode, 409);
  f.advance();
  assert.equal((await f.service.review(created.body.post_id, 'approve', 'admin')).statusCode, 200);
  const visible = await f.service.handle('GET', '/v1/video-circle/feed', {}, f.viewer);
  assert.equal(visible.body.posts.length, 1);
  assert.equal(visible.body.posts[0].author_id, 'author');
  assert.equal(visible.body.posts[0].object_key, undefined);
  const publicPlay = await f.service.handle('GET',
    `/v1/video-circle/posts/${created.body.post_id}/play`, {}, f.viewer);
  assert.equal(publicPlay.statusCode, 302);
  assert.match(publicPlay.headers.Location, /^https:\/\/private-oss\.example\//);

  const report = await f.service.handle('POST',
    `/v1/video-circle/posts/${created.body.post_id}/report`,
    { reason: 'copyright', details: '未经授权的素材' }, f.viewer);
  assert.equal(report.statusCode, 201);
  assert.equal(f.db.prepare('SELECT COUNT(*) AS count FROM video_circle_reports').get().count, 1);
  assert.equal((await f.service.handle('POST', '/v1/video-circle/blocks',
    { author_id: 'author' }, f.viewer)).statusCode, 200);
  assert.deepEqual((await f.service.handle('GET', '/v1/video-circle/feed',
    {}, f.viewer)).body.posts, []);
  assert.equal((await f.service.handle('GET',
    `/v1/video-circle/posts/${created.body.post_id}/play`, {}, f.viewer)).statusCode, 404);
  assert.equal((await f.service.handle('POST', '/v1/video-circle/blocks/remove',
    { author_id: 'author' }, f.viewer)).statusCode, 200);
  assert.equal((await f.service.handle('GET', '/v1/video-circle/feed',
    {}, f.viewer)).body.posts.length, 1);
  const hidden = await f.service.hide(created.body.post_id, 'admin', '审核后下架');
  assert.equal(hidden.statusCode, 200);
  assert.deepEqual(f.removed, [f.key]);
  assert.deepEqual((await f.service.handle('GET', '/v1/video-circle/feed',
    {}, f.viewer)).body.posts, []);
});

test('withdraw removes feed access and requests object deletion', async () => {
  const f = fixture();
  const created = await f.service.handle('POST', '/v1/video-circle/posts', f.post, f.owner);
  f.advance();
  await f.service.review(created.body.post_id, 'approve', 'admin');
  const denied = await f.service.handle('POST',
    `/v1/video-circle/posts/${created.body.post_id}/withdraw`, {}, f.viewer);
  assert.equal(denied.statusCode, 404);
  const withdrawn = await f.service.handle('POST',
    `/v1/video-circle/posts/${created.body.post_id}/withdraw`, {}, f.owner);
  assert.equal(withdrawn.statusCode, 200);
  assert.deepEqual(f.removed, [f.key]);
  assert.deepEqual((await f.service.handle('GET', '/v1/video-circle/feed',
    {}, f.viewer)).body.posts, []);
  assert.equal((await f.service.handle('GET',
    `/v1/video-circle/posts/${created.body.post_id}/play`, {}, f.viewer)).statusCode, 404);
});

test('withdrawal during read signing never returns a fresh playback link', async () => {
  const f = fixture();
  const created = await f.service.handle('POST', '/v1/video-circle/posts', f.post, f.owner);
  f.advance();
  await f.service.review(created.body.post_id, 'approve', 'admin');
  let finishSigning;
  const signing = new Promise(resolve => { finishSigning = resolve; });
  const raceService = createVideoCircleService({ db: f.db,
    signRead: async () => { await signing; return 'https://private.example/old-link'; },
    deleteObject: async () => {} });
  const play = raceService.handle('GET',
    `/v1/video-circle/posts/${created.body.post_id}/play-link`, {}, f.viewer);
  await raceService.handle('POST',
    `/v1/video-circle/posts/${created.body.post_id}/withdraw`, {}, f.owner);
  finishSigning();
  assert.equal((await play).statusCode, 404);
});

test('failed object deletion hides a video and leaves a retryable cleanup item', async () => {
  const f = fixture();
  const created = await f.service.handle('POST', '/v1/video-circle/posts', f.post, f.owner);
  f.advance();
  await f.service.review(created.body.post_id, 'approve', 'admin');
  const unavailableStorage = createVideoCircleService({ db: f.db,
    deleteObject: async () => { throw new Error('OSS unavailable'); } });
  const withdrawn = await unavailableStorage.handle('POST',
    `/v1/video-circle/posts/${created.body.post_id}/withdraw`, {}, f.owner);
  assert.equal(withdrawn.statusCode, 202);
  assert.equal(withdrawn.body.cleanup_pending, true);
  assert.equal((await f.service.handle('GET',
    `/v1/video-circle/posts/${created.body.post_id}/play-link`, {}, f.viewer))
    .statusCode, 404);
  assert.equal(f.service.pendingCleanup().length, 1);
  assert.equal((await f.service.retryCleanup(created.body.post_id)).statusCode, 200);
  assert.deepEqual(f.removed, [f.key]);
  assert.deepEqual(f.service.pendingCleanup(), []);
});

test('rejection requires a reason and resolved reports record the reviewer', async () => {
  const f = fixture();
  const created = await f.service.handle('POST', '/v1/video-circle/posts', f.post, f.owner);
  assert.equal((await f.service.review(created.body.post_id, 'reject', 'operator'))
    .statusCode, 400);
  f.advance();
  await f.service.review(created.body.post_id, 'approve', 'operator');
  const report = await f.service.handle('POST',
    `/v1/video-circle/posts/${created.body.post_id}/report`,
    { reason: 'spam' }, f.viewer);
  assert.equal(f.service.resolveReport(report.body.report_id, 'operator', 'Reviewed')
    .statusCode, 200);
  assert.equal(f.db.prepare('SELECT reviewer FROM video_circle_reports WHERE id=?')
    .get(report.body.report_id).reviewer, 'operator');
});

test('an unconfigured verifier never accepts an uploaded video', async () => {
  const f = fixture();
  const service = createVideoCircleService({ db: f.db });
  const response = await service.handle('POST', '/v1/video-circle/posts', f.post, f.owner);
  assert.equal(response.statusCode, 503);
  assert.equal(f.db.prepare('SELECT COUNT(*) AS count FROM video_circle_posts').get().count, 0);
  assert.equal((await service.handle('GET', '/v1/video-circle/feed', {}, null)).statusCode, 401);
});

test('a disabled account cannot finish an upload while object verification is in flight', async () => {
  const f = fixture();
  let releaseVerification;
  const gate = new Promise(resolve => { releaseVerification = resolve; });
  const service = createVideoCircleService({ db: f.db,
    verifyUpload: async () => { await gate; return { exists: true,
      size_bytes: 1000, content_type: 'video/mp4' }; } });
  const submitted = service.handle('POST', '/v1/video-circle/posts', f.post, f.owner);
  f.db.prepare('UPDATE users SET disabled_at=2 WHERE id=?').run(f.owner.user_id);
  releaseVerification();
  assert.equal((await submitted).statusCode, 401);
  assert.equal(f.db.prepare('SELECT COUNT(*) AS count FROM video_circle_posts').get().count, 0);
});

test('account HTTP routes are off by default and require a valid account token when enabled', async () => {
  const db = openStore(':memory:');
  const base = {
    db, sms: { async send() {} },
    secret: 'test-only-auth-hmac-key-keep-on-server-123456',
    imSecret: 'test-only-tencent-im-key-keep-on-server-123456',
    sdkAppId: 123456789
  };
  const disabled = createAuthService(base);
  assert.equal((await disabled.handle({ method: 'GET', path: '/v1/video-circle/feed' }))
    .statusCode, 404);
  const enabled = createAuthService({ ...base, videoCircle: {
    verifyUpload: async () => ({ exists: true, size_bytes: 1000,
      content_type: 'video/mp4' }),
    signRead: async key => `https://private-oss.example/${key}`,
    deleteObject: async () => {}
  } });
  const server = createServer(enabled);
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  try {
    const origin = `http://127.0.0.1:${server.address().port}`;
    assert.equal((await fetch(`${origin}/v1/video-circle/feed`)).status, 401);
    assert.equal((await enabled.createTestAccount('circle_author',
      'circle-password-123456789')).statusCode, 201);
    const login = await fetch(`${origin}/v1/auth/password/login`, {
      method: 'POST', headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ account_id: 'circle_author',
        password: 'circle-password-123456789', device_slot: 'ios_phone',
        age_confirmed_14_plus: true })
    });
    assert.equal(login.status, 200);
    const { access_token: token } = await login.json();
    const feed = await fetch(`${origin}/v1/video-circle/feed`, {
      headers: { Authorization: `Bearer ${token}` }
    });
    assert.equal(feed.status, 200);
    assert.deepEqual((await feed.json()).posts, []);
  } finally {
    await new Promise(resolve => server.close(resolve));
    db.close();
  }
});

test('account deletion hides and deletes published videos before returning', async () => {
  const db = openStore(':memory:');
  const removed = [];
  const service = createAuthService({
    db, sms: { async send() {} },
    secret: 'test-only-auth-hmac-key-keep-on-server-123456',
    imSecret: 'test-only-tencent-im-key-keep-on-server-123456',
    sdkAppId: 123456789,
    videoCircle: { verifyUpload: async () => ({ exists: true, size_bytes: 1000,
      content_type: 'video/mp4' }), signRead: async () => 'https://private.example/read',
    deleteObject: async key => { removed.push(key); } }
  });
  assert.equal((await service.createTestAccount('delete_author',
    'delete-author-password-123456789')).statusCode, 201);
  const login = await service.handle({ method: 'POST', path: '/v1/auth/password/login',
    body: { account_id: 'delete_author', password: 'delete-author-password-123456789',
      device_slot: 'ios_phone', age_confirmed_14_plus: true },
    ip: '192.0.2.1', headers: {} });
  assert.equal(login.statusCode, 200);
  const key = `video-circle/delete_author/2026-10-22/${randomUUID()}.mp4`;
  const id = randomUUID();
  db.prepare(`INSERT INTO video_circle_posts
    (id,author_user_id,title,object_key,source_tool,source_task_id,size_bytes,
    content_type,duration_ms,status,created_at,published_at)
    VALUES(?,?,?,?,'wan_video','task-1',1000,'video/mp4',8000,'approved',1,2)`)
    .run(id,'delete_author','旧视频',key);
  const deleted = await service.handle({ method: 'POST',
    path: '/v1/auth/account/deletion',
    body: { confirm_account_id: 'delete_author',
      password: 'delete-author-password-123456789' },
    ip: '192.0.2.1',
    headers: { authorization: `Bearer ${login.body.access_token}` } });
  assert.equal(deleted.statusCode, 202);
  assert.equal(deleted.body.video_cleanup_pending, false);
  assert.deepEqual(removed, [key]);
  assert.deepEqual(db.prepare('SELECT status,cleanup_pending FROM video_circle_posts WHERE id=?')
    .get(id), { status: 'withdrawn', cleanup_pending: 0 });
  db.close();
});
