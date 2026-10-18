'use strict';

const http = require('node:http');
const { createHash, timingSafeEqual } = require('node:crypto');

function escapeHtml(value) {
  return String(value).replace(/[&<>"']/g, character => ({
    '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;'
  })[character]);
}

function authenticated(header, secret) {
  if (typeof header !== 'string' || !header.startsWith('Basic ')) return false;
  let decoded;
  try { decoded = Buffer.from(header.slice(6), 'base64').toString('utf8'); }
  catch { return false; }
  const expected = createHash('sha256').update(`admin:${secret}`).digest();
  const actual = createHash('sha256').update(decoded).digest();
  return timingSafeEqual(actual, expected);
}

function page(db, message = '') {
  const accounts = db.prepare(`SELECT id,created_at,disabled_at FROM users
    WHERE phone_e164 IS NULL ORDER BY created_at DESC`).all();
  const rows = accounts.map(account => `<tr><td>${escapeHtml(account.id)}</td><td>` +
    `${account.disabled_at ? '已停用' : '可用'}</td><td>` +
    (account.disabled_at ? '' : `<form method="post" action="/disable">
      <input type="hidden" name="account_id" value="${escapeHtml(account.id)}">
      <button type="submit">停用</button></form>`) + '</td></tr>').join('');
  return `<!doctype html><html lang="zh-CN"><head><meta charset="utf-8">
    <meta name="viewport" content="width=device-width,initial-scale=1">
    <title>MaiChat 测试账号</title><style>
    body{font:16px system-ui,sans-serif;max-width:720px;margin:40px auto;padding:0 20px;color:#172033}
    form{display:grid;gap:12px}label{display:grid;gap:5px}input,button{font:inherit;padding:10px}
    table{border-collapse:collapse;width:100%;margin-top:24px}
    td,th{border-bottom:1px solid #ddd;padding:10px;text-align:left}
    .message{padding:12px;background:#eef5ff}</style></head><body>
    <h1>MaiChat 测试账号</h1><p>仅经 SSH 转发访问；手机号留空，账号同时用作腾讯 IM ID。</p>
    ${message ? `<p class="message">${escapeHtml(message)}</p>` : ''}
    <form method="post" action="/create" autocomplete="off">
      <label>MaiChat 账号 ID<input name="account_id" required maxlength="32" pattern="[a-z][a-z0-9_-]{2,31}"></label>
      <label>密码（至少 15 字符）<input name="password" type="password" required minlength="15" maxlength="128"></label>
      <button type="submit">创建测试账号</button></form>
    <h2>测试账号</h2><table><tr><th>MaiChat ID</th><th>状态</th><th>操作</th></tr>${rows}</table>
    </body></html>`;
}

function createAdminServer({ db, service, secret }) {
  if (!db || !service || typeof secret !== 'string' || secret.length < 32)
    throw new Error('Admin UI configuration is incomplete');
  return http.createServer(async (request, response) => {
    try {
    response.setHeader('Cache-Control', 'no-store');
    response.setHeader('X-Content-Type-Options', 'nosniff');
    response.setHeader('X-Frame-Options', 'DENY');
    response.setHeader('Content-Security-Policy', "default-src 'none'; style-src 'unsafe-inline'; form-action 'self'");
    if (!authenticated(request.headers.authorization, secret)) {
      response.writeHead(401, { 'WWW-Authenticate': 'Basic realm="MaiChat Admin"' });
      response.end('Authentication required');
      return;
    }
    if (request.method === 'GET' && request.url === '/') {
      response.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8' });
      response.end(page(db));
      return;
    }
    const origin = request.headers.origin;
    const host = request.headers.host;
    let originUrl;
    try { originUrl = new URL(origin); } catch { originUrl = null; }
    if (request.method !== 'POST' || !['/create', '/disable'].includes(request.url) ||
        !originUrl || !host || originUrl.host !== host ||
        !['127.0.0.1', 'localhost'].includes(originUrl.hostname) ||
        originUrl.protocol !== 'http:' ||
        request.headers['content-type']?.split(';')[0].trim().toLowerCase() !==
          'application/x-www-form-urlencoded') {
      response.writeHead(403);
      response.end('Forbidden');
      return;
    }
    let bytes = 0;
    const chunks = [];
    for await (const chunk of request) {
      bytes += chunk.length;
      if (bytes > 4096) {
        response.writeHead(413);
        response.end('Request is too large');
        return;
      }
      chunks.push(chunk);
    }
    const form = new URLSearchParams(Buffer.concat(chunks, bytes).toString('utf8'));
    const accountId = form.get('account_id');
    const result = request.url === '/create' ?
      await service.createTestAccount(accountId, form.get('password')) :
      service.disableTestAccount(accountId);
    response.writeHead(result.statusCode < 400 ? 200 : result.statusCode,
      { 'Content-Type': 'text/html; charset=utf-8' });
    response.end(page(db, result.statusCode < 400 ? '操作成功' : result.body.error));
    } catch {
      if (!response.headersSent) response.writeHead(500);
      response.end('Admin operation failed');
    }
  });
}

module.exports = { createAdminServer };
