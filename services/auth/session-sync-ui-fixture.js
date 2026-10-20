'use strict';

// Local simulator fixture: one MaiChat test account and a fake Mac Session.
// It never calls a model or reaches Tencent IM. Bind only to loopback.
const http = require('node:http');
const https = require('node:https');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { execFileSync } = require('node:child_process');
const { randomUUID, createHash } = require('node:crypto');
const { openStore } = require('./store');
const { createAuthService } = require('./service');
const { openSessionSyncStore } = require('./session-sync-store');
const { createSessionSyncService } = require('./session-sync-service');
const { attachWebSocket, createAccountAuthorizer } = require('./session-sync-server');

const port = 18190;
const socketPort = 18191;
const accountID = 'sync-ui-test';
const password = 'local sync UI test passphrase';
const sessionID = 'ses_mock_mac';
const authDb = openStore(':memory:');
const syncDb = openSessionSyncStore(':memory:');
const auth = createAuthService({
  db: authDb,
  sms: { async send() {} },
  secret: 'local-sync-ui-auth-hmac-secret-123456789',
  imSecret: 'local-sync-ui-im-secret-1234567890123',
  sdkAppId: 123456789
});
const sync = createSessionSyncService({ db: syncDb,
  authorize: createAccountAuthorizer(`http://127.0.0.1:${port}`) });
let macToken;
const messages = [{ id: 'msg_0001', role: 'assistant', created: 1,
  completed: 2, active: false,
  parts: [{ id: 'part_0001', kind: 'text', text: '这条历史来自本机假 Mac Session。' }] }];

async function call(token, method, path, body = {}) {
  return sync.handle({ method, path, body,
    headers: { authorization: `Bearer ${token}` } });
}

async function sendPayload(to, kind, value) {
  const data = Buffer.from(JSON.stringify(value));
  const digest = createHash('sha256').update(data).digest('hex');
  const transferId = randomUUID().replaceAll('-', '');
  const chunkSize = 24 * 1024;
  const count = Math.max(1, Math.ceil(data.length / chunkSize));
  for (let index = 0; index < count; index++) {
    const payload = { transfer_id: transferId, chunk_index: index,
      chunk_count: count, sha256: digest,
      data_base64: data.subarray(index * chunkSize, (index + 1) * chunkSize).toString('base64') };
    const sent = await call(macToken, 'POST', '/v1/session-sync/send', {
      to_slot: to.from_slot, to_login_session_id: to.from_login_session_id,
      client_message_id: randomUUID().replaceAll('-', ''),
      session_id: sessionID, kind, payload
    });
    if (sent.statusCode !== 200) throw new Error(`Fixture send failed: ${sent.statusCode}`);
  }
}

async function fakeMacLoop() {
  while (true) {
    const result = await call(macToken, 'GET', '/v1/session-sync/poll?wait_ms=1000');
    if (result.statusCode !== 200) throw new Error(`Fixture poll failed: ${result.statusCode}`);
    for (const envelope of result.body.messages) {
      if (envelope.kind === 'history_request') {
        await sendPayload(envelope, 'history_page', { ok: true, messages });
      } else if (envelope.kind === 'command' && envelope.payload.action === 'prompt') {
        const text = envelope.payload.text;
        if (text === '请拒绝此操作') {
          await sendPayload(envelope, 'event', { type: 'command_result',
            command_id: envelope.client_message_id,
            result: { ok: false, error: '所属设备拒绝了操作' } });
          await call(macToken, 'POST', '/v1/session-sync/ack', { ids: [envelope.id] });
          continue;
        }
        const id = String(messages.length + 1).padStart(4, '0');
        messages.push({ id: `msg_${id}`, role: 'user', created: messages.length + 1,
          completed: messages.length + 2, active: false,
          parts: [{ id: `part_${id}`, kind: 'text', text }] });
        await sendPayload(envelope, 'event', { type: 'command_result',
          command_id: envelope.client_message_id, result: { ok: true } });
        await sendPayload(envelope, 'event', { ok: true, messages });
      }
      await call(macToken, 'POST', '/v1/session-sync/ack', { ids: [envelope.id] });
    }
  }
}

async function start() {
  const certificateDirectory = fs.mkdtempSync(path.join(os.tmpdir(), 'maichat-sync-ui-'));
  const certificatePath = path.join(certificateDirectory, 'localhost.pem');
  const keyPath = path.join(certificateDirectory, 'localhost.key');
  execFileSync('openssl', ['req', '-x509', '-newkey', 'rsa:2048', '-sha256', '-nodes',
    '-keyout', keyPath, '-out', certificatePath, '-days', '1',
    '-subj', '/CN=localhost',
    '-addext', 'subjectAltName=DNS:localhost,IP:127.0.0.1,IP:10.0.2.2'],
  { stdio: 'ignore' });
  const certificate = fs.readFileSync(certificatePath);
  const key = fs.readFileSync(keyPath);
  fs.rmSync(certificateDirectory, { recursive: true, force: true });
  const created = await auth.createTestAccount(accountID, password);
  if (created.statusCode !== 201) throw new Error('Could not create local test account');
  const login = await auth.handle({ method: 'POST', path: '/v1/auth/password/login',
    ip: '127.0.0.1', body: { account_id: accountID, password,
      age_confirmed_14_plus: true, device_slot: 'mac' } });
  if (login.statusCode !== 200) throw new Error('Could not log in fake Mac');
  macToken = login.body.access_token;
  const server = http.createServer(async (request, response) => {
    if (request.url === '/test/cert') {
      response.writeHead(200, { 'Content-Type': 'application/x-pem-file' });
      response.end(certificate);
      return;
    }
    if (request.url === '/test/status') {
      const devices = syncDb.prepare('SELECT slot,last_seen FROM sync_devices').all();
      const envelopes = syncDb.prepare(
        'SELECT kind,from_slot,to_slot,acked_at FROM sync_envelopes ORDER BY id').all();
      response.writeHead(200, { 'Content-Type': 'application/json' });
      response.end(JSON.stringify({ slots: devices.map(device => device.slot),
        messages: messages.length, envelopes }));
      return;
    }
    const chunks = [];
    for await (const chunk of request) chunks.push(chunk);
    let body = {};
    try { if (chunks.length) body = JSON.parse(Buffer.concat(chunks).toString('utf8')); }
    catch {
      response.writeHead(400);
      response.end(JSON.stringify({ error: 'Invalid JSON' }));
      return;
    }
    const isSync = request.url.startsWith('/v1/session-sync/');
    const result = isSync ? await sync.handle({ method: request.method,
      path: request.url, body, headers: request.headers }) :
      await auth.handle({ method: request.method, path: request.url,
        body, headers: request.headers, ip: '127.0.0.1' });
    response.writeHead(result.statusCode, { 'Content-Type': 'application/json' });
    response.end(JSON.stringify(result.body));
  });
  await new Promise(resolve => server.listen(port, '127.0.0.1', resolve));
  const socketServer = attachWebSocket(
    https.createServer({ key, cert: certificate }, (_request, response) => {
      response.writeHead(404);
      response.end();
    }), sync, createAccountAuthorizer(`http://127.0.0.1:${port}`)
  );
  await new Promise(resolve => socketServer.listen(socketPort, '127.0.0.1', resolve));
  const present = await call(macToken, 'POST', '/v1/session-sync/presence', {
    device_id: 'localmacfixture', name: '测试 Mac',
    sessions: [{ id: sessionID, title: 'Mac 本地测试任务',
      updated_at: Date.now(), busy: false }]
  });
  if (present.statusCode !== 200) throw new Error('Could not publish fake Mac Session');
  setInterval(() => call(macToken, 'POST', '/v1/session-sync/presence', {
    device_id: 'localmacfixture', name: '测试 Mac',
    sessions: [{ id: sessionID, title: 'Mac 本地测试任务',
      updated_at: Date.now(), busy: false }]
  }), 15_000);
  fakeMacLoop().catch(error => { process.stderr.write(`${error.message}\n`); process.exitCode = 1; });
  process.stdout.write(`Local Session sync fixture HTTP ${port}, WSS ${socketPort}\n`);
}

start().catch(error => { process.stderr.write(`${error.message}\n`); process.exitCode = 1; });
