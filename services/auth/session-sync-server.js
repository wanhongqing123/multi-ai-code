'use strict';

const http = require('node:http');
const { WebSocket, WebSocketServer } = require('ws');
const { openSessionSyncStore } = require('./session-sync-store');
const { createSessionSyncService } = require('./session-sync-service');

const MAX_BODY_BYTES = 96 * 1024;

function createServer(service, authorize = async () => null) {
  const server = http.createServer(async (request, response) => {
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
  return attachWebSocket(server, service, authorize);
}

function attachWebSocket(server, service, authorize = async () => null) {
  const sockets = new Map();
  const upgrades = new WebSocketServer({ noServer: true, clientTracking: false,
    maxPayload: MAX_BODY_BYTES, perMessageDeflate: false });

  function rejectUpgrade(socket, status) {
    socket.end(`HTTP/1.1 ${status}\r\nConnection: close\r\nContent-Length: 0\r\n\r\n`);
  }
  function send(socket, value) {
    if (socket.readyState !== WebSocket.OPEN) return false;
    if (socket.bufferedAmount > 1024 * 1024) {
      socket.close(1013, 'Device is not receiving messages');
      return false;
    }
    socket.send(JSON.stringify(value));
    return true;
  }
  function connect(socket, request, principal) {
    const route = `${principal.accountId}:${principal.slot}:${principal.loginSessionId}`;
    const previous = sockets.get(route);
    if (previous && previous !== socket) previous.close(4001, 'Device connection replaced');
    sockets.set(route, socket);
    let alive = true;
    let announced = false;
    let pushing = false;
    let pushAgain = false;
    let windowStarted = Date.now();
    let framesInWindow = 0;
    const headers = { ...request.headers };
    const call = (method, path, body = {}) => service.handle({ method, path, body, headers });
    async function pushCatalog() {
      if (socket.readyState !== WebSocket.OPEN) return;
      const result = await call('GET', '/v1/session-sync/sessions');
      if (socket.readyState !== WebSocket.OPEN) return;
      if (result.statusCode !== 200) { socket.close(1008, 'Account session expired'); return; }
      send(socket, { type: 'catalog', sessions: result.body.sessions,
        devices: result.body.devices });
    }
    async function pushInbox() {
      if (!announced) return;
      if (pushing) { pushAgain = true; return; }
      pushing = true;
      try {
        do {
          pushAgain = false;
          if (socket.readyState !== WebSocket.OPEN) return;
          const result = await call('GET', '/v1/session-sync/poll?wait_ms=0');
          if (result.statusCode === 409 || result.statusCode === 401) {
            socket.close(1008, 'Device login replaced');
            return;
          }
          if (result.statusCode !== 200) return;
          if (result.body.messages.length)
            send(socket, { type: 'inbox', messages: result.body.messages });
        } while (pushAgain && socket.readyState === WebSocket.OPEN);
      } finally { pushing = false; }
    }
    const unsubscribe = service.subscribe(principal,
      () => { void pushInbox().catch(() => socket.close(1011)); },
      () => {
        if (announced) void pushCatalog().catch(() => socket.close(1011));
      });
    const heartbeat = setInterval(() => {
      if (!alive) { socket.terminate(); return; }
      alive = false;
      socket.ping();
    }, 25000);
    socket.on('pong', () => { alive = true; });
    socket.on('close', () => {
      clearInterval(heartbeat);
      unsubscribe();
      if (sockets.get(route) === socket) {
        sockets.delete(route);
        service.markDisconnected(principal);
      }
    });
    let processing = Promise.resolve();
    socket.on('message', (data, binary) => {
      processing = processing.then(async () => {
        if (binary) { socket.close(1003, 'Text frames only'); return; }
        if (Date.now() - windowStarted >= 60_000) {
          windowStarted = Date.now();
          framesInWindow = 0;
        }
        if (++framesInWindow > 600) { socket.close(1008, 'Device request rate exceeded'); return; }
        let frame;
        try { frame = JSON.parse(data.toString('utf8')); }
        catch { socket.close(1007, 'Invalid JSON'); return; }
        const id = frame?.id;
        const op = frame?.op;
        if (typeof id !== 'string' || !/^[A-Za-z0-9_-]{1,64}$/.test(id) ||
            !['presence', 'catalog', 'send', 'ack', 'publish', 'history',
              'room-delete', 'reauth'].includes(op) ||
            !frame.body || typeof frame.body !== 'object' || Array.isArray(frame.body)) {
          socket.close(1008, 'Invalid device request');
          return;
        }
        if (op === 'reauth') {
          const token = frame.body.access_token;
          if (typeof token !== 'string' || !/^[A-Za-z0-9_-]{43}$/.test(token)) {
            socket.close(1008, 'Invalid reauthorization');
            return;
          }
          let next;
          try { next = await authorize({ authorization: `Bearer ${token}` }); }
          catch {
            send(socket, { type: 'response', id, status: 503,
              body: { error: 'Account service is unavailable' } });
            return;
          }
          if (!next || next.accountId !== principal.accountId ||
              next.slot !== principal.slot ||
              next.loginSessionId !== principal.loginSessionId) {
            socket.close(1008, 'Device login replaced');
            return;
          }
          headers.authorization = `Bearer ${token}`;
          send(socket, { type: 'response', id, status: 200, body: {} });
          return;
        }
        if (!announced && op !== 'presence') {
          send(socket, { type: 'response', id, status: 409,
            body: { error: 'Device presence is required' } });
          return;
        }
        const method = op === 'catalog' || op === 'history' ? 'GET' : 'POST';
        const path = op === 'history'
          ? `/v1/session-sync/history?${new URLSearchParams(frame.body)}`
          : `/v1/session-sync/${op === 'catalog' ? 'sessions' : op}`;
        const result = await call(method, path, frame.body);
        send(socket, { type: 'response', id, status: result.statusCode, body: result.body });
        if (op === 'presence' && result.statusCode === 200) {
          announced = true;
          await pushInbox();
          await pushCatalog();
        }
        if (result.statusCode === 401 || result.statusCode === 403)
          socket.close(1008, 'Account session expired');
      }).catch(() => socket.close(1011, 'Device request failed'));
    });
  }
  server.on('upgrade', async (request, socket, head) => {
    const path = new URL(request.url, 'http://localhost').pathname;
    if (path !== '/v1/session-sync/ws') { rejectUpgrade(socket, '404 Not Found'); return; }
    try {
      const principal = await authorize(request.headers);
      if (!principal) { rejectUpgrade(socket, '401 Unauthorized'); return; }
      if (!principal.accountId || !principal.slot || !principal.loginSessionId) {
        rejectUpgrade(socket, '403 Forbidden');
        return;
      }
      upgrades.handleUpgrade(request, socket, head,
        connection => connect(connection, request, principal));
    } catch { rejectUpgrade(socket, '503 Service Unavailable'); }
  });
  return server;
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
  const authorize = createAccountAuthorizer(`http://127.0.0.1:${authPort}`);
  const service = createSessionSyncService({ db, authorize });
  const server = createServer(service, authorize);
  const maintenance = setInterval(() => service.cleanup(), 60_000);
  maintenance.unref();
  server.on('close', () => { clearInterval(maintenance); db.close(); });
  server.listen(port, '127.0.0.1');
  return server;
}

if (require.main === module) start();

module.exports = { createServer, attachWebSocket, createAccountAuthorizer, start };
