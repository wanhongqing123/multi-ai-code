'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const { openStore } = require('./store');
const { createAuthService } = require('./service');
const { createServer: createAuthServer, videoCircleFromEnv } = require('./server');
const { createHandler: createMediaHandler } = require('../oss-media-signer/index');
const { createServer: createMediaServer } = require('../oss-media-signer/server');

test('two accounts see an AI video only after private upload verification and review', async () => {
  const db = openStore(':memory:');
  const secret = 'circle-integration-internal-key-at-least-32-characters';
  const keyId = '123e4567-e89b-42d3-a456-426614174000';
  const deleted = [];
  let clock = Date.now();
  let authService;
  const mediaHandler = createMediaHandler({
    env: {
      MAICHAT_OSS_BUCKET: 'private-test-bucket',
      MAICHAT_OSS_REGION: 'oss-cn-hangzhou',
      ALIBABA_CLOUD_ACCESS_KEY_ID: 'local-test-id',
      ALIBABA_CLOUD_ACCESS_KEY_SECRET: 'local-test-secret',
      MAICHAT_VIDEO_CIRCLE_ENABLED: '1',
      MAICHAT_VIDEO_CIRCLE_INTERNAL_KEY: secret
    },
    newID: () => keyId,
    currentTime: () => new Date('2026-10-22T00:00:00Z'),
    authorizeMediaRequest: async headers => authService?.authenticate(headers)?.account_id || false,
    makeClient: () => ({
      signatureUrlV4: async (method, _ttl, _options, key) =>
        `https://private-oss.example/${key}?method=${method}`,
      head: async () => ({ res: { headers: { 'content-length': '1000',
        'content-type': 'video/mp4' } } }),
      delete: async key => { deleted.push(key); }
    })
  });
  const mediaServer = createMediaServer(mediaHandler);
  await new Promise(resolve => mediaServer.listen(0, '127.0.0.1', resolve));
  const mediaOrigin = `http://127.0.0.1:${mediaServer.address().port}`;
  authService = createAuthService({
    db, sms: { async send() {} }, now: () => clock,
    secret: 'test-only-auth-hmac-key-keep-on-server-123456',
    imSecret: 'test-only-tencent-im-key-keep-on-server-123456', sdkAppId: 123456789,
    videoCircle: videoCircleFromEnv({ MAICHAT_VIDEO_CIRCLE_ENABLED: '1',
      MAICHAT_VIDEO_CIRCLE_INTERNAL_KEY: secret,
      MAICHAT_MEDIA_PORT: String(mediaServer.address().port) })
  });
  const authServer = createAuthServer(authService);
  await new Promise(resolve => authServer.listen(0, '127.0.0.1', resolve));
  try {
    const authOrigin = `http://127.0.0.1:${authServer.address().port}`;
    for (const id of ['circle_author', 'circle_viewer']) {
      assert.equal((await authService.createTestAccount(id,
        `${id}-password-123456789`)).statusCode, 201);
    }
    async function login(id) {
      const response = await fetch(`${authOrigin}/v1/auth/password/login`, {
        method: 'POST', headers: { 'content-type': 'application/json' },
        body: JSON.stringify({ account_id: id, password: `${id}-password-123456789`,
          device_slot: 'ios_phone', age_confirmed_14_plus: true })
      });
      assert.equal(response.status, 200);
      return (await response.json()).access_token;
    }
    const authorToken = await login('circle_author');
    const viewerToken = await login('circle_viewer');
    const upload = await fetch(`${mediaOrigin}/sign-upload`, {
      method: 'POST', headers: { Authorization: `Bearer ${authorToken}`,
        'content-type': 'application/json' },
      body: JSON.stringify({ filename: 'clip.mp4', size_bytes: 1000,
        purpose: 'video_circle' })
    });
    assert.equal(upload.status, 200);
    const signed = await upload.json();
    assert.equal(signed.read_url, undefined);
    assert.equal(signed.object_key,
      `video-circle/circle_author/2026-10-22/${keyId}.mp4`);
    const submit = await fetch(`${authOrigin}/v1/video-circle/posts`, {
      method: 'POST', headers: { Authorization: `Bearer ${authorToken}`,
        'content-type': 'application/json' },
      body: JSON.stringify({ title: '海边', object_key: signed.object_key,
        source_tool: 'wan_video', source_task_id: 'task-1', size_bytes: 1000,
        duration_ms: 8000, ai_generated: true })
    });
    assert.equal(submit.status, 201);
    const { post_id: postId } = await submit.json();
    const viewerHeaders = { Authorization: `Bearer ${viewerToken}` };
    assert.deepEqual((await (await fetch(`${authOrigin}/v1/video-circle/feed`, {
      headers: viewerHeaders
    })).json()).posts, []);
    assert.equal((await authService.videoCircleAdmin.review(postId, 'approve', 'admin'))
      .statusCode, 409);
    clock += 16 * 60_000;
    assert.equal((await authService.videoCircleAdmin.review(postId, 'approve', 'admin')).statusCode,
      200);
    const feed = await (await fetch(`${authOrigin}/v1/video-circle/feed`, {
      headers: viewerHeaders
    })).json();
    assert.equal(feed.posts.length, 1);
    const play = await fetch(`${authOrigin}/v1/video-circle/posts/${postId}/play`, {
      headers: viewerHeaders, redirect: 'manual'
    });
    assert.equal(play.status, 302);
    assert.match(play.headers.get('location'), /method=GET$/);
    const playLink = await fetch(`${authOrigin}/v1/video-circle/posts/${postId}/play-link`, {
      headers: viewerHeaders
    });
    assert.equal(playLink.status, 200);
    assert.equal((await playLink.json()).read_expires_in, 60);
    const withdrawn = await fetch(`${authOrigin}/v1/video-circle/posts/${postId}/withdraw`, {
      method: 'POST', headers: { Authorization: `Bearer ${authorToken}` }
    });
    assert.equal(withdrawn.status, 200);
    assert.deepEqual(deleted, [signed.object_key]);
    assert.equal((await fetch(`${authOrigin}/v1/video-circle/posts/${postId}/play`, {
      headers: viewerHeaders
    })).status, 404);
  } finally {
    await new Promise(resolve => authServer.close(resolve));
    await new Promise(resolve => mediaServer.close(resolve));
    db.close();
  }
});
