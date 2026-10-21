'use strict';

const http = require('node:http');
const { openStore } = require('./store');
const { createAuthService } = require('./service');
const { createAliyunSmsSender } = require('./sms');
const { createArkCredentialIssuer } = require('./ark-credential');
const { createBailianCredentialIssuer } = require('./bailian-credential');
const { createSocialVerifiers } = require('./social-providers');
const { createAccountModelKeyIssuer } = require('./account-model-keys');
const { createAsrRealtimeUrlIssuer } = require('./asr-realtime-url');
const { createAdminServer } = require('./admin-server');
const { createTencentFriendWriter, syncPendingFriendActions } = require('./tim-friend-sync');
const { createTencentProfileWriter, syncPendingProfileActions } =
  require('./tim-profile-sync');
const { createTencentAccountProvisioner } = require('./tim-account-provision');

const MAX_BODY_BYTES = 4096;

function requireDeviceSlotFromEnv(value) {
  if (value === undefined || value === '' || value === '0') return false;
  if (value === '1') return true;
  throw new Error('MAICHAT_AUTH_REQUIRE_DEVICE_SLOT must be 0 or 1');
}

function videoCircleFromEnv(env, fetcher = fetch) {
  if (env.MAICHAT_VIDEO_CIRCLE_ENABLED === undefined ||
      env.MAICHAT_VIDEO_CIRCLE_ENABLED === '0') return null;
  if (env.MAICHAT_VIDEO_CIRCLE_ENABLED !== '1')
    throw new Error('MAICHAT_VIDEO_CIRCLE_ENABLED must be 0 or 1');
  const secret = env.MAICHAT_VIDEO_CIRCLE_INTERNAL_KEY || '';
  const port = Number(env.MAICHAT_MEDIA_PORT || 9000);
  if (secret.length < 32 || !Number.isInteger(port) || port < 1 || port > 65535)
    throw new Error('Video circle internal signer is not configured');
  const request = async (action, body) => {
    const response = await fetcher(`http://127.0.0.1:${port}/video-circle/${action}`, {
      method: 'POST', redirect: 'error', signal: AbortSignal.timeout(5000),
      headers: { 'Content-Type': 'application/json',
        'X-MaiChat-Video-Circle-Key': secret }, body: JSON.stringify(body)
    });
    if (response.status !== 200) throw new Error('Video circle signer rejected the request');
    return response.json();
  };
  return {
    verifyUpload: async (accountId, objectKey, bytes) =>
      request('verify', { account_id: accountId, object_key: objectKey,
        expected_bytes: bytes }),
    signRead: async objectKey =>
      (await request('read', { object_key: objectKey })).read_url,
    deleteObject: async objectKey => {
      const reply = await request('delete', { object_key: objectKey });
      if (reply.deleted !== true) throw new Error('Video object was not deleted');
    }
  };
}

function createServer(service) {
  return http.createServer(async (request, response) => {
    response.setHeader('Content-Type', 'application/json; charset=utf-8');
    response.setHeader('Cache-Control', 'no-store');
    response.setHeader('X-Content-Type-Options', 'nosniff');
    try {
      if (request.method === 'GET' && request.url === '/health') {
        response.end(JSON.stringify({ ok: true }));
        return;
      }
      let bytes = 0;
      const chunks = [];
      for await (const chunk of request) {
        bytes += chunk.length;
        if (bytes > MAX_BODY_BYTES) {
          response.writeHead(413);
          response.end(JSON.stringify({ error: 'Request is too large' }));
          return;
        }
        chunks.push(chunk);
      }
      let body = {};
      if (bytes) {
        try { body = JSON.parse(Buffer.concat(chunks, bytes).toString('utf8')); }
        catch {
          response.writeHead(400);
          response.end(JSON.stringify({ error: 'Invalid JSON' }));
          return;
        }
      }
      const url = new URL(request.url, 'http://localhost');
      const path = url.pathname;
      // 本进程只监听 loopback。Nginx 必须覆盖 X-Real-IP，不能透传客户端自填值。
      const ip = request.headers['x-real-ip'] || request.socket.remoteAddress;
      const result = await service.handle({ method: request.method, path, body,
                                            headers: request.headers, ip,
                                            query: url.searchParams });
      response.writeHead(result.statusCode, result.headers || {});
      response.end(result.statusCode === 302 ? '' : JSON.stringify(result.body));
    } catch {
      if (!response.headersSent) response.writeHead(500);
      response.end(JSON.stringify({ error: 'Internal service error' }));
    }
  });
}

function start(env = process.env) {
  const requireDeviceSlot = requireDeviceSlotFromEnv(env.MAICHAT_AUTH_REQUIRE_DEVICE_SLOT);
  if (env.MAICHAT_VIDEO_CIRCLE_ENABLED === '1' &&
      (!env.MAICHAT_AUTH_ADMIN_SECRET || env.MAICHAT_AUTH_ADMIN_SECRET.length < 32))
    throw new Error('Video circle requires an authenticated admin review server');
  if (!env.MAICHAT_AUTH_DB_PATH || !env.MAICHAT_AUTH_HMAC_KEY ||
      !env.MAICHAT_IM_SECRET_KEY || !env.MAICHAT_IM_SDK_APP_ID)
    throw new Error('Auth database or server signing secrets are missing');
  const db = openStore(env.MAICHAT_AUTH_DB_PATH);
  const sms = createAliyunSmsSender(env);
  const service = createAuthService({
    db, sms, secret: env.MAICHAT_AUTH_HMAC_KEY,
    imSecret: env.MAICHAT_IM_SECRET_KEY,
    videoCircle: videoCircleFromEnv(env),
    issueArkCredential: createArkCredentialIssuer(env),
    issueBailianCredential: createBailianCredentialIssuer(env),
    issueAccountModelKeys: createAccountModelKeyIssuer(env),
    issueAsrRealtimeUrl: createAsrRealtimeUrlIssuer(env),
    socialVerifiers: createSocialVerifiers(env),
    sdkAppId: Number(env.MAICHAT_IM_SDK_APP_ID),
    provisionTestImAccount: createTencentAccountProvisioner({
      sdkAppId: Number(env.MAICHAT_IM_SDK_APP_ID),
      imSecret: env.MAICHAT_IM_SECRET_KEY,
      adminUser: env.MAICHAT_IM_ADMIN_USER || 'administrator',
      origin: env.MAICHAT_IM_REST_ORIGIN || 'https://console.tim.qq.com'
    }),
    requireDeviceSlot
  });
  const friendWriter = createTencentFriendWriter({
    sdkAppId: Number(env.MAICHAT_IM_SDK_APP_ID), imSecret: env.MAICHAT_IM_SECRET_KEY,
    adminUser: env.MAICHAT_IM_ADMIN_USER || 'administrator',
    origin: env.MAICHAT_IM_REST_ORIGIN || 'https://console.tim.qq.com'
  });
  const profileWriter = createTencentProfileWriter({
    sdkAppId: Number(env.MAICHAT_IM_SDK_APP_ID), imSecret: env.MAICHAT_IM_SECRET_KEY,
    adminUser: env.MAICHAT_IM_ADMIN_USER || 'administrator',
    origin: env.MAICHAT_IM_REST_ORIGIN || 'https://console.tim.qq.com'
  });
  const port = Number(env.MAICHAT_AUTH_PORT || 9100);
  if (!Number.isInteger(port) || port < 1 || port > 65535)
    throw new Error('Invalid auth port');
  // 即使长期无人登录，也按天清理过期安全记录；重启时先补做一次。
  service.cleanupExpiredRecords();
  const maintenance = setInterval(() => service.cleanupExpiredRecords(),
    24 * 60 * 60 * 1000);
  maintenance.unref();
  let friendSyncRunning = false;
  const friendSync = setInterval(async () => {
    if (friendSyncRunning) return;
    friendSyncRunning = true;
    try { await syncPendingFriendActions(db, friendWriter); }
    catch { process.stderr.write('MaiChat friend IM sync failed\n'); }
    finally { friendSyncRunning = false; }
  }, 5_000);
  friendSync.unref();
  let profileSyncRunning = false;
  const profileSync = setInterval(async () => {
    if (profileSyncRunning) return;
    profileSyncRunning = true;
    try { await syncPendingProfileActions(db, profileWriter); }
    catch { process.stderr.write('MaiChat profile IM sync failed\n'); }
    finally { profileSyncRunning = false; }
  }, 5_000);
  profileSync.unref();
  const server = createServer(service);
  server.on('close', () => {
    clearInterval(maintenance); clearInterval(friendSync); clearInterval(profileSync);
  });
  server.listen(port, '127.0.0.1');
  if (env.MAICHAT_AUTH_ADMIN_SECRET) {
    const adminPort = Number(env.MAICHAT_AUTH_ADMIN_PORT || 9101);
    if (!Number.isInteger(adminPort) || adminPort < 1 || adminPort > 65535 || adminPort === port)
      throw new Error('Invalid admin port');
    const adminServer = createAdminServer({ db, service,
      secret: env.MAICHAT_AUTH_ADMIN_SECRET });
    adminServer.listen(adminPort, '127.0.0.1');
    server.on('close', () => adminServer.close());
  }
  return server;
}

if (require.main === module) start();

module.exports = { createServer, start, requireDeviceSlotFromEnv, videoCircleFromEnv };
