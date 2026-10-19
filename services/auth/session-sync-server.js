'use strict';

const http = require('node:http');
const { openSessionSyncStore } = require('./session-sync-store');
const { createSessionSyncService } = require('./session-sync-service');

const MAX_BODY_BYTES = 96 * 1024;

function createServer(service) {
  return http.createServer(async (request, response) => {
    response.setHeader('Content-Type', 'application/json; charset=utf-8');
    response.setHeader('Cache-Control', 'no-store');
    response.setHeader('X-Content-Type-Options', 'nosniff');
    try {
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
      if (request.method === 'GET' && path === '/health') {
        response.end(JSON.stringify({ ok: true }));
        return;
      }
      const result = await service.handle({ method: request.method,
        path: request.url, body, headers: request.headers });
      response.writeHead(result.statusCode);
      response.end(JSON.stringify(result.body));
    } catch (error) {
      if (!response.headersSent)
        response.writeHead(error?.code === 'AUTH_UNAVAILABLE' ? 503 : 500);
      response.end(JSON.stringify({ error: error?.code === 'AUTH_UNAVAILABLE'
        ? 'Account service is unavailable' : 'Internal service error' }));
    }
  });
}

function createAccountAuthorizer(origin, fetcher = fetch) {
  return async headers => {
    const bearer = headers?.authorization;
    if (typeof bearer !== 'string' || !/^Bearer [A-Za-z0-9_-]{43}$/.test(bearer))
      return null;
    try {
      const response = await fetcher(`${origin}/v1/auth/me`, {
        headers: { Authorization: bearer }, redirect: 'error',
        signal: AbortSignal.timeout(5000)
      });
      if (response.status === 401) return null;
      if (response.status !== 200) throw new Error('Account service is unavailable');
      const body = await response.json();
      return { accountId: body.user?.id, slot: body.device_slot,
        loginSessionId: body.login_session_id };
    } catch {
      const error = new Error('Account service is unavailable');
      error.code = 'AUTH_UNAVAILABLE';
      throw error;
    }
  };
}

function start(env = process.env) {
  const dbPath = env.MAICHAT_SESSION_SYNC_DB_PATH;
  const port = Number(env.MAICHAT_SESSION_SYNC_PORT || 9102);
  const authPort = Number(env.MAICHAT_AUTH_PORT || 9100);
  if (!dbPath || !Number.isInteger(port) || port < 1 || port > 65535 ||
      !Number.isInteger(authPort) || authPort < 1 || authPort > 65535 ||
      port === authPort)
    throw new Error('Session sync configuration is incomplete');
  const db = openSessionSyncStore(dbPath);
  const service = createSessionSyncService({ db,
    authorize: createAccountAuthorizer(`http://127.0.0.1:${authPort}`) });
  const server = createServer(service);
  const maintenance = setInterval(() => service.cleanup(), 60_000);
  maintenance.unref();
  server.on('close', () => { clearInterval(maintenance); db.close(); });
  server.listen(port, '127.0.0.1');
  return server;
}

if (require.main === module) start();

module.exports = { createServer, createAccountAuthorizer, start };
