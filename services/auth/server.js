'use strict';

const http = require('node:http');
const { openStore } = require('./store');
const { createAuthService, normalizePhone } = require('./service');
const { createAliyunSmsSender } = require('./sms');
const { createArkCredentialIssuer } = require('./ark-credential');
const { createBailianCredentialIssuer } = require('./bailian-credential');

const MAX_BODY_BYTES = 4096;

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
      const path = new URL(request.url, 'http://localhost').pathname;
      // 本进程只监听 loopback。Nginx 必须覆盖 X-Real-IP，不能透传客户端自填值。
      const ip = request.headers['x-real-ip'] || request.socket.remoteAddress;
      const result = await service.handle({ method: request.method, path, body,
                                            headers: request.headers, ip });
      response.writeHead(result.statusCode);
      response.end(JSON.stringify(result.body));
    } catch {
      if (!response.headersSent) response.writeHead(500);
      response.end(JSON.stringify({ error: 'Internal service error' }));
    }
  });
}

function start(env = process.env) {
  const allowedPhones = (env.MAICHAT_AUTH_ALLOWED_PHONES || '').split(',')
    .map(normalizePhone).filter(Boolean);
  const modelAllowedPhones = (env.MAICHAT_MODEL_ALLOWED_PHONES || '').split(',')
    .map(normalizePhone).filter(Boolean);
  if (!env.MAICHAT_AUTH_DB_PATH || !env.MAICHAT_AUTH_HMAC_KEY ||
      !env.MAICHAT_IM_SECRET_KEY || !env.MAICHAT_IM_SDK_APP_ID)
    throw new Error('Auth database or server signing secrets are missing');
  const db = openStore(env.MAICHAT_AUTH_DB_PATH);
  const sms = createAliyunSmsSender(env);
  const service = createAuthService({
    db, sms, secret: env.MAICHAT_AUTH_HMAC_KEY,
    imSecret: env.MAICHAT_IM_SECRET_KEY,
    issueArkCredential: createArkCredentialIssuer(env),
    issueBailianCredential: createBailianCredentialIssuer(env),
    sdkAppId: Number(env.MAICHAT_IM_SDK_APP_ID), allowedPhones,
    openRegistration: env.MAICHAT_AUTH_OPEN_REGISTRATION === '1',
    modelAllowedPhones
  });
  const port = Number(env.MAICHAT_AUTH_PORT || 9100);
  if (!Number.isInteger(port) || port < 1 || port > 65535)
    throw new Error('Invalid auth port');
  // 即使长期无人登录，也按天清理过期安全记录；重启时先补做一次。
  service.cleanupExpiredRecords();
  const maintenance = setInterval(() => service.cleanupExpiredRecords(),
    24 * 60 * 60 * 1000);
  maintenance.unref();
  const server = createServer(service);
  server.on('close', () => clearInterval(maintenance));
  server.listen(port, '127.0.0.1');
  return server;
}

if (require.main === module) start();

module.exports = { createServer, start };
