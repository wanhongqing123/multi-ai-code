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

const port = Number(process.env.MAICHAT_SYNC_UI_AUTH_PORT || 18190);
const socketPort = Number(process.env.MAICHAT_SYNC_UI_SOCKET_PORT || 18191);
const modelPort = Number(process.env.MAICHAT_SYNC_UI_MODEL_PORT || 18189);
const circlePort = Number(process.env.MAICHAT_SYNC_UI_CIRCLE_PORT || 18444);
const accountID = 'sync-ui-test';
const viewerAccountID = 'sync-ui-viewer';
const password = 'local sync UI test passphrase';
const sessionID = 'ses_mock_mac';
const authDb = openStore(':memory:');
const syncDb = openSessionSyncStore(':memory:');
let fixtureTime = Date.now();
const circleObjects = new Map();
const circleUploadKeys = new Map();
const auth = createAuthService({
  db: authDb,
  sms: { async send() {} },
  secret: 'local-sync-ui-auth-hmac-secret-123456789',
  imSecret: 'local-sync-ui-im-secret-1234567890123',
  sdkAppId: 123456789,
  now: () => fixtureTime,
  videoCircle: {
    verifyUpload: async (owner, key) => {
      const object = circleObjects.get(key);
      return object && key.startsWith(`video-circle/${owner}/`)
        ? { exists: true, size_bytes: object.length, content_type: 'video/mp4' }
        : { exists: false };
    },
    signRead: async key => circleObjects.has(key)
      ? `https://localhost:${circlePort}/object/${encodeURIComponent(key)}` : null,
    deleteObject: async key => { circleObjects.delete(key); }
  }
});
const sync = createSessionSyncService({ db: syncDb,
  authorize: createAccountAuthorizer(`http://127.0.0.1:${port}`) });
let macToken;
let phoneSession;
let macCreatedCount = 0;
let macSessions = [{ id: sessionID, title: 'Mac 本地测试任务',
  updated_at: Date.now(), busy: false }];
const workspaceSwitches = [];
let macPermissions = [];
const macPermissionDecisions = [];
const modelCalls = [];
const modelRequests = [];
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
      if (envelope.kind === 'command' && envelope.session_id === 'session_lifecycle') {
        let lifecycleResult;
        if (envelope.payload.action === 'session_create') {
          const createdID = `ses_remote_created_${++macCreatedCount}`;
          macSessions = [{ id: createdID, title: 'New session',
            updated_at: Date.now(), busy: false }, ...macSessions];
          lifecycleResult = { ok: true, session_id: createdID };
        } else if (envelope.payload.action === 'session_delete') {
          const target = envelope.payload.target_session_id;
          macSessions = macSessions.filter(session => session.id !== target);
          await call(macToken, 'POST', '/v1/session-sync/room-delete', { session_id: target });
          lifecycleResult = { ok: true, session_id: target };
        } else if (envelope.payload.action === 'session_set_workspace') {
          const target = envelope.payload.target_session_id;
          const workspacePath = envelope.payload.workspace_path;
          if (!macSessions.some(session => session.id === target) ||
              !path.isAbsolute(workspacePath || '') ||
              !fs.existsSync(workspacePath)) {
            lifecycleResult = { ok: false, error: 'Workspace 不存在' };
          } else {
            workspaceSwitches.push({ session_id: target, workspace_path: workspacePath });
            lifecycleResult = { ok: true, session_id: target };
          }
        } else lifecycleResult = { ok: false, error: '不支持的操作' };
        const sent = await call(macToken, 'POST', '/v1/session-sync/send', {
          to_slot: envelope.from_slot,
          to_login_session_id: envelope.from_login_session_id,
          client_message_id: randomUUID().replaceAll('-', ''),
          session_id: 'session_lifecycle', kind: 'event',
          payload: { type: 'command_result', command_id: envelope.client_message_id,
            result: lifecycleResult }
        });
        if (sent.statusCode !== 200)
          throw new Error(`Fixture lifecycle result failed: ${sent.statusCode}`);
        await call(macToken, 'POST', '/v1/session-sync/presence', {
          device_id: 'localmacfixture', name: '测试 Mac', sessions: macSessions
        });
        await call(macToken, 'POST', '/v1/session-sync/ack', { ids: [envelope.id] });
        continue;
      }
      if (envelope.kind === 'command' && envelope.payload.action === 'prompt') {
        const text = envelope.payload.text;
        if (text === '从 iPhone 请求本机假 Mac')
          await new Promise(resolve => setTimeout(resolve, 2000));
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
        await call(macToken, 'POST', '/v1/session-sync/publish', {
          session_id: sessionID, messages: [messages[messages.length - 1]],
          permissions: macPermissions, questions: []
        });
      }
      if (envelope.kind === 'command' && envelope.payload.action === 'permission') {
        macPermissionDecisions.push(envelope.payload.decision);
        const valid = macPermissions.some(permission => permission.id === envelope.payload.id) &&
          ['approved', 'approved_for_session', 'denied'].includes(envelope.payload.decision);
        if (valid) macPermissions = [];
        await sendPayload(envelope, 'event', { type: 'command_result',
          command_id: envelope.client_message_id,
          result: valid ? { ok: true } : { ok: false, error: '权限决定无效' } });
        if (valid) await call(macToken, 'POST', '/v1/session-sync/publish', {
          session_id: sessionID, messages: [], permissions: macPermissions, questions: []
        });
      }
      await call(macToken, 'POST', '/v1/session-sync/ack', { ids: [envelope.id] });
    }
  }
}

async function targetPhone(kind, payload) {
  const catalog = await call(macToken, 'GET', '/v1/session-sync/sessions');
  const phone = catalog.body.sessions.find(entry => entry.device_slot === 'ios_phone' &&
    entry.online && entry.id !== sessionID);
  if (!phone) return { statusCode: 404, body: { error: 'Phone Session unavailable' } };
  phoneSession = phone;
  if (kind === 'history') return call(macToken, 'GET',
    `/v1/session-sync/history?owner_slot=ios_phone&session_id=${phone.id}&limit=10`);
  return call(macToken, 'POST', '/v1/session-sync/send', {
    to_slot: phone.device_slot, to_login_session_id: phone.login_session_id,
    client_message_id: randomUUID().replaceAll('-', ''),
    session_id: phone.id, kind, payload
  });
}

async function targetPhoneLifecycle(action, targetSessionID = '') {
  const catalog = await call(macToken, 'GET', '/v1/session-sync/sessions');
  const phone = catalog.body.devices.find(entry => entry.device_slot === 'ios_phone' &&
    entry.online);
  if (!phone) return { statusCode: 404, body: { error: 'Phone device unavailable' } };
  const commandID = randomUUID().replaceAll('-', '');
  const payload = action === 'session_create'
    ? { action } : { action, target_session_id: targetSessionID };
  const sent = await call(macToken, 'POST', '/v1/session-sync/send', {
    to_slot: phone.device_slot, to_login_session_id: phone.login_session_id,
    client_message_id: commandID, session_id: 'session_lifecycle',
    kind: 'command', payload
  });
  return { statusCode: sent.statusCode,
    body: { ...sent.body, command_id: commandID } };
}

function phoneLifecycleResults() {
  return syncDb.prepare(`SELECT payload FROM sync_envelopes
    WHERE from_slot='ios_phone' AND to_slot='mac' AND kind='event'
      AND session_id='session_lifecycle' ORDER BY id`).all()
    .map(row => JSON.parse(row.payload))
    .filter(payload => payload.type === 'command_result');
}

function decodedPhoneEvents() {
  const rows = syncDb.prepare(`SELECT kind,from_slot,payload FROM sync_envelopes
    WHERE from_slot='ios_phone' AND to_slot='mac' ORDER BY id`).all();
  const transfers = new Map();
  for (const row of rows) {
    if (row.kind !== 'event') continue;
    const payload = JSON.parse(row.payload);
    const id = payload.transfer_id;
    if (!id || !Number.isInteger(payload.chunk_count)) continue;
    const transfer = transfers.get(id) || { kind: row.kind, count: payload.chunk_count,
      chunks: [] };
    transfer.chunks[payload.chunk_index] = payload.data_base64;
    transfers.set(id, transfer);
  }
  return [...transfers.values()].filter(item => item.chunks.filter(Boolean).length === item.count)
    .map(item => {
      try { return { kind: item.kind, body: JSON.parse(Buffer.concat(item.chunks.map(
        chunk => Buffer.from(chunk, 'base64'))).toString('utf8')) }; }
      catch { return null; }
    }).filter(Boolean);
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
  const viewerCreated = await auth.createTestAccount(viewerAccountID, password);
  if (viewerCreated.statusCode !== 201) throw new Error('Could not create local viewer account');
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
        messages: messages.length, envelopes, phoneSession: phoneSession?.id,
        phoneEvents: decodedPhoneEvents(), modelCalls, modelRequests,
        macSessions: macSessions.map(session => session.id), workspaceSwitches,
        macPermissionDecisions,
        phoneLifecycleResults: phoneLifecycleResults(),
        phoneSessions: syncDb.prepare(`SELECT session_id FROM sync_sessions
          WHERE slot='ios_phone'`).all().map(row => row.session_id) }));
      return;
    }
    if (request.url === '/test/video-circle/status') {
      const posts = authDb.prepare(`SELECT id,title,status,author_user_id FROM
        video_circle_posts ORDER BY created_at`).all();
      response.writeHead(200, { 'Content-Type': 'application/json' });
      response.end(JSON.stringify({ posts, objects: circleObjects.size }));
      return;
    }
    if (request.url === '/test/video-circle/source') {
      const sample = path.resolve(__dirname,
        '../../MaiChat/ios/MaiChat/MaiChatUITests/Fixtures/video-circle-source.mp4');
      response.writeHead(200, { 'Content-Type': 'video/mp4' });
      response.end(fs.readFileSync(sample));
      return;
    }
    if (request.url.startsWith('/test/video-circle/approve')) {
      const url = new URL(request.url, 'http://localhost');
      const postID = url.searchParams.get('post_id') || '';
      fixtureTime += 16 * 60_000;
      const result = await auth.videoCircleAdmin.review(postID, 'approve', 'fixture-admin');
      response.writeHead(result.statusCode, { 'Content-Type': 'application/json' });
      response.end(JSON.stringify(result.body));
      return;
    }
    if (request.url === '/test/mac-permission') {
      macPermissions = [{ id: 'permission_delete_remote_session',
        tool: 'maichat_remote_session_delete',
        input: JSON.stringify({ device_slot: 'ios_phone', session_id: 'ses_test_delete' }),
        allowForSession: false, rememberOnApproval: false, fileCount: 0 }];
      const published = await call(macToken, 'POST', '/v1/session-sync/publish', {
        session_id: sessionID, messages: [], permissions: macPermissions, questions: []
      });
      response.writeHead(published.statusCode, { 'Content-Type': 'application/json' });
      response.end(JSON.stringify(published.body));
      return;
    }
    if (request.url.startsWith('/test/phone-lifecycle')) {
      const url = new URL(request.url, 'http://localhost');
      const result = await targetPhoneLifecycle(
        url.searchParams.get('action') || '',
        url.searchParams.get('target') || '');
      response.writeHead(result.statusCode, { 'Content-Type': 'application/json' });
      response.end(JSON.stringify(result.body));
      return;
    }
    if (request.url === '/test/phone-subscribe' ||
        request.url.startsWith('/test/phone-prompt')) {
      const subscribe = request.url === '/test/phone-subscribe';
      const prompt = new URL(request.url, 'http://localhost').searchParams.get('text') || '你好';
      const result = await targetPhone(subscribe ? 'history' : 'command',
        subscribe ? { limit: 10 } : { action: 'prompt', text: prompt });
      response.writeHead(result.statusCode, { 'Content-Type': 'application/json' });
      response.end(JSON.stringify(result.body));
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
  const modelServer = http.createServer(async (request, response) => {
    const chunks = [];
    for await (const chunk of request) chunks.push(chunk);
    let input = [];
    try {
      const body = JSON.parse(Buffer.concat(chunks).toString('utf8'));
      input = Array.isArray(body.input) ? body.input : [];
    } catch { /* The fake model can still return a fixed answer. */ }
    let latestUser = -1;
    for (let index = 0; index < input.length; index++)
      if (input[index]?.role === 'user') latestUser = index;
    const latestText = latestUser >= 0 ? JSON.stringify(input[latestUser]) : '';
    const toolOutput = input.slice(latestUser + 1).some(item =>
      item?.type === 'function_call_output');
    const toolOutputText = input.slice(latestUser + 1).filter(item =>
      item?.type === 'function_call_output').at(-1)?.output || '';
    const cameraAction = !toolOutput ?
      ['open', 'status', 'close'].find(action => latestText.includes(`camera-${action}`)) : null;
    const remoteCreateTool = !toolOutput &&
      latestText.includes('remote-session-create-tool');
    const remoteDeleteTool = !toolOutput &&
      latestText.includes('remote-session-delete-tool');
    const remoteWorkspaceTool = !toolOutput &&
      latestText.includes('remote-session-workspace-tool');
    modelRequests.push({ latestText: latestText.slice(0, 180), toolOutput,
      cameraAction: cameraAction || '', toolOutputText: String(toolOutputText).slice(0, 300),
      inputTypes: input.map(item => item?.type || item?.role || '') });
    if (cameraAction) modelCalls.push(cameraAction);
    if (remoteCreateTool) modelCalls.push('remote_session_create');
    if (remoteDeleteTool) modelCalls.push('remote_session_delete');
    if (remoteWorkspaceTool) modelCalls.push('remote_session_set_workspace');
    const marker = cameraAction ||
      ['open', 'status', 'close'].find(action => latestText.includes(`camera-${action}`));
    response.writeHead(200, { 'Content-Type': 'text/event-stream' });
    const events = [{ type: 'response.created' }];
    if (cameraAction || remoteCreateTool || remoteDeleteTool || remoteWorkspaceTool) {
      const call = cameraAction
        ? { type: 'function_call', call_id: `camera_${randomUUID().replaceAll('-', '')}`,
          name: 'maichat_camera_command', arguments: JSON.stringify({ action: cameraAction }) }
        : remoteDeleteTool
          ? { type: 'function_call', call_id: `remote_${randomUUID().replaceAll('-', '')}`,
            name: 'maichat_remote_session_delete', arguments: JSON.stringify({
              device_slot: 'mac', session_id: sessionID }) }
        : remoteWorkspaceTool
          ? { type: 'function_call', call_id: `remote_${randomUUID().replaceAll('-', '')}`,
            name: 'maichat_remote_session_set_workspace', arguments: JSON.stringify({
              device_slot: 'mac', session_id: sessionID, workspace_path: '/tmp' }) }
          : { type: 'function_call', call_id: `remote_${randomUUID().replaceAll('-', '')}`,
          name: 'maichat_remote_session_create',
          arguments: JSON.stringify({ device_slot: 'mac' }) };
      events.push({ type: 'response.output_item.added', output_index: 0, item: call });
      events.push({ type: 'response.function_call_arguments.done', output_index: 0,
        arguments: call.arguments });
      events.push({ type: 'response.completed', response: {
        status: 'completed', output: [call] } });
    } else {
      const answer = marker ? `CAMERA_${marker.toUpperCase()}_DONE` :
        latestText.includes('remote-session-create-tool')
          ? 'REMOTE_CREATE_TOOL_DONE' :
          latestText.includes('remote-session-delete-tool')
            ? 'REMOTE_DELETE_TOOL_DONE' :
          latestText.includes('remote-session-workspace-tool')
            ? 'REMOTE_WORKSPACE_TOOL_DONE' : '收到';
      events.push({ type: 'response.output_text.delta', output_index: 0,
        content_index: 0, delta: answer });
      events.push({ type: 'response.completed', response: { status: 'completed', output: [
        { type: 'message', content: [{ type: 'output_text', text: answer }] }
      ] } });
    }
    for (const event of events) response.write(`data: ${JSON.stringify(event)}\n\n`);
    response.end();
  });
  await new Promise(resolve => modelServer.listen(modelPort, '127.0.0.1', resolve));
  const socketServer = attachWebSocket(
    https.createServer({ key, cert: certificate }, (_request, response) => {
      response.writeHead(404);
      response.end();
    }), sync, createAccountAuthorizer(`http://127.0.0.1:${port}`)
  );
  await new Promise(resolve => socketServer.listen(socketPort, '127.0.0.1', resolve));
  const circleServer = https.createServer({ key, cert: certificate },
    async (request, response) => {
      const url = new URL(request.url, `https://localhost:${circlePort}`);
      const chunks = [];
      for await (const chunk of request) chunks.push(chunk);
      const data = Buffer.concat(chunks);
      const send = (status, body, headers = {}) => {
        response.writeHead(status, { 'Content-Type': 'application/json', ...headers });
        response.end(typeof body === 'string' ? body : JSON.stringify(body));
      };
      if (request.method === 'GET' && url.pathname.startsWith('/object/')) {
        const object = circleObjects.get(decodeURIComponent(url.pathname.slice(8)));
        if (!object) return send(404, { error: 'Not found' });
        response.writeHead(200, { 'Content-Type': 'video/mp4' });
        response.end(object);
        return;
      }
      if (request.method === 'PUT' && url.pathname.startsWith('/video-circle/')) {
        const ticket = url.searchParams.get('fixture_upload') || '';
        const key = circleUploadKeys.get(ticket);
        if (!key || url.pathname.slice(1) !== key || circleObjects.has(key) ||
            request.headers['x-oss-forbid-overwrite'] !== 'true' ||
            request.headers['content-type'] !== 'video/mp4')
          return send(409, { error: 'Invalid or duplicate upload' });
        circleObjects.set(key, data);
        circleUploadKeys.delete(ticket);
        response.writeHead(200);
        response.end();
        return;
      }
      if (request.method === 'POST' && url.pathname === '/sign-upload') {
        const me = await auth.handle({ method: 'GET', path: '/v1/auth/me',
          headers: request.headers, ip: '127.0.0.1' });
        if (me.statusCode !== 200) return send(401, { error: 'Authentication required' });
        let input;
        try { input = JSON.parse(data.toString('utf8')); }
        catch { return send(400, { error: 'Invalid JSON' }); }
        if (input.purpose !== 'video_circle' || input.size_bytes < 1 ||
            input.size_bytes > 200_000_000 || input.filename !== 'video.mp4')
          return send(400, { error: 'Invalid upload' });
        const id = randomUUID();
        const date = new Date(fixtureTime).toISOString().slice(0, 10);
        const objectKey = `video-circle/${me.body.user.id}/${date}/${id}.mp4`;
        circleUploadKeys.set(id, objectKey);
        return send(200, { upload_url:
          `https://localhost:${circlePort}/${objectKey}?fixture_upload=${id}`,
          upload_headers: { 'Content-Type': 'video/mp4',
            'x-oss-forbid-overwrite': 'true' },
          object_key: objectKey, upload_expires_in: 900 });
      }
      let body = {};
      try { if (data.length) body = JSON.parse(data.toString('utf8')); }
      catch { return send(400, { error: 'Invalid JSON' }); }
      const result = await auth.handle({ method: request.method,
        path: url.pathname, query: url.searchParams, body,
        headers: request.headers, ip: '127.0.0.1' });
      send(result.statusCode, result.body, result.headers);
    });
  await new Promise(resolve => circleServer.listen(circlePort, '127.0.0.1', resolve));
  const present = await call(macToken, 'POST', '/v1/session-sync/presence', {
    device_id: 'localmacfixture', name: '测试 Mac',
    sessions: macSessions
  });
  if (present.statusCode !== 200) throw new Error('Could not publish fake Mac Session');
  const seeded = await call(macToken, 'POST', '/v1/session-sync/publish', {
    session_id: sessionID, messages, permissions: [], questions: []
  });
  if (seeded.statusCode !== 200) throw new Error('Could not publish fake Mac history');
  setInterval(() => call(macToken, 'POST', '/v1/session-sync/presence', {
    device_id: 'localmacfixture', name: '测试 Mac',
    sessions: macSessions
  }), 15_000);
  fakeMacLoop().catch(error => { process.stderr.write(`${error.message}\n`); process.exitCode = 1; });
  process.stdout.write(`Local Session sync fixture HTTP ${port}, WSS ${socketPort}, video circle TLS ${circlePort}\n`);
}

start().catch(error => { process.stderr.write(`${error.message}\n`); process.exitCode = 1; });
