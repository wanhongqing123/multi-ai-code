'use strict';

const assert = require('node:assert/strict');
const { randomBytes } = require('node:crypto');
const test = require('node:test');
const { openStore } = require('./store');
const { createAuthService } = require('./service');
const { createAdminServer } = require('./admin-server');

test('local admin page requires authentication and same-origin POST', async () => {
  const db = openStore(':memory:');
  const service = createAuthService({ db, sms: { async send() {} },
    secret: randomBytes(32).toString('hex'), imSecret: randomBytes(32).toString('hex'),
    sdkAppId: 123456789 });
  const server = createAdminServer({ db, service, secret: 'admin-test-secret-at-least-32-chars' });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  try {
    const base = `http://127.0.0.1:${server.address().port}`;
    const authorization = 'Basic ' + Buffer.from(
      'admin:admin-test-secret-at-least-32-chars').toString('base64');
    assert.equal((await fetch(base)).status, 401);
    assert.equal((await fetch(base, { headers: { authorization } })).status, 200);
    const body = new URLSearchParams({ account_id: 'test_admin',
      password: 'admin-password-123456789' });
    assert.equal((await fetch(`${base}/create`, { method: 'POST',
      headers: { authorization, 'content-type': 'application/x-www-form-urlencoded',
        origin: 'https://example.invalid' }, body })).status, 403);
    assert.equal(db.prepare('SELECT COUNT(*) AS count FROM users').get().count, 0);
    assert.equal((await fetch(`${base}/create`, { method: 'POST',
      headers: { authorization, 'content-type': 'application/x-www-form-urlencoded',
        origin: base }, body })).status, 200);
    assert.equal(db.prepare('SELECT phone_e164,im_user_id FROM users WHERE id=?')
      .get('test_admin').im_user_id, 'test_admin');
    const html = await (await fetch(base, { headers: { authorization } })).text();
    assert.equal(html.includes('admin-password-123456789'), false);
    assert.equal((await service.createTestAccount('report_target',
      'report-target-password-123456')).statusCode, 201);
    const login = await service.handle({ method: 'POST', path: '/v1/auth/password/login',
      body: { account_id: 'test_admin', password: 'admin-password-123456789',
        age_confirmed_14_plus: true, device_slot: 'ios_phone' },
      ip: '192.0.2.1', headers: {} });
    assert.equal(login.statusCode, 200);
    const reported = await service.handle({ method: 'POST', path: '/v1/safety/reports',
      body: { target_im_user_id: 'report_target', reason: 'spam' },
      ip: '192.0.2.1',
      headers: { authorization: `Bearer ${login.body.access_token}` } });
    assert.equal(reported.statusCode, 201);
    const pending = await (await fetch(base, { headers: { authorization } })).text();
    assert.match(pending, /report_target/);
    const resolution = new URLSearchParams({ report_id: reported.body.report_id,
      resolution: 'Reviewed and action taken' });
    assert.equal((await fetch(`${base}/resolve-report`, { method: 'POST',
      headers: { authorization, 'content-type': 'application/x-www-form-urlencoded',
        origin: base }, body: resolution })).status, 200);
    const own = await service.handle({ method: 'GET', path: '/v1/safety/reports',
      body: {}, ip: '192.0.2.1',
      headers: { authorization: `Bearer ${login.body.access_token}` } });
    assert.equal(own.body.reports[0].status, 'resolved');
  } finally {
    await new Promise(resolve => server.close(resolve));
    db.close();
  }
});

test('local video review requires a checked decision and can hide published media', async () => {
  const db = openStore(':memory:');
  db.prepare(`INSERT INTO users
    (id,account_id,phone_e164,im_user_id,created_at,updated_at)
    VALUES('author','author',NULL,'author-im',1,1)`).run();
  const postId = '123e4567-e89b-42d3-a456-426614174001';
  const objectKey = 'video-circle/author/2026-10-22/123e4567-e89b-42d3-a456-426614174000.mp4';
  db.prepare(`INSERT INTO video_circle_posts
    (id,author_user_id,title,object_key,source_tool,source_task_id,size_bytes,
    content_type,duration_ms,status,created_at)
    VALUES(?,?,?,?,'wan_video','task-1',1000,'video/mp4',8000,'pending',1)`)
    .run(postId, 'author', '<unsafe> video', objectKey);
  const removed = [];
  const service = createAuthService({ db, sms: { async send() {} },
    secret: randomBytes(32).toString('hex'), imSecret: randomBytes(32).toString('hex'),
    sdkAppId: 123456789, videoCircle: {
      verifyUpload: async () => ({ exists: true, size_bytes: 1000,
        content_type: 'video/mp4' }),
      signRead: async () => 'https://private.example/review.mp4?expires=60',
      deleteObject: async key => { removed.push(key); }
    } });
  const server = createAdminServer({ db, service, secret: 'admin-test-secret-at-least-32-chars' });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  try {
    const base = `http://127.0.0.1:${server.address().port}`;
    const authorization = 'Basic ' + Buffer.from(
      'admin:admin-test-secret-at-least-32-chars').toString('base64');
    const headers = { authorization, 'content-type': 'application/x-www-form-urlencoded',
      origin: base };
    const html = await (await fetch(base, { headers: { authorization } })).text();
    assert.match(html, /AI 视频圈 · 待审核/);
    assert.match(html, /&lt;unsafe&gt; video/);
    assert.ok(!html.includes('<unsafe>'));
    assert.equal((await fetch(`${base}/video-circle/preview/${postId}`)).status, 401);
    const preview = await fetch(`${base}/video-circle/preview/${postId}`, {
      headers: { authorization }, redirect: 'manual'
    });
    assert.equal(preview.status, 302);
    assert.equal(preview.headers.get('location'),
      'https://private.example/review.mp4?expires=60');
    const review = body => fetch(`${base}/video-circle/review`, {
      method: 'POST', headers, body: new URLSearchParams(body)
    });
    assert.equal((await review({ post_id: postId, decision: 'approve' })).status, 400);
    assert.equal((await review({ post_id: postId, decision: 'approve', checked: 'yes' })).status, 200);
    assert.equal(db.prepare('SELECT status FROM video_circle_posts WHERE id=?').get(postId).status,
      'approved');
    const hidden = await fetch(`${base}/video-circle/hide`, {
      method: 'POST', headers, body: new URLSearchParams({ post_id: postId,
        reason: '审核后下架' })
    });
    assert.equal(hidden.status, 200);
    assert.equal(db.prepare('SELECT status FROM video_circle_posts WHERE id=?').get(postId).status,
      'hidden');
    assert.deepEqual(removed, [objectKey]);
  } finally {
    await new Promise(resolve => server.close(resolve));
    db.close();
  }
});
