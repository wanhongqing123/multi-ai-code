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

function page(db, videoCircle, message = '') {
  const accounts = db.prepare(`SELECT id,created_at,disabled_at FROM users
    WHERE phone_e164 IS NULL ORDER BY created_at DESC`).all();
  const rows = accounts.map(account => `<tr><td>${escapeHtml(account.id)}</td><td>` +
    `${account.disabled_at ? '已停用' : '可用'}</td><td>` +
    (account.disabled_at ? '' : `<form method="post" action="/disable">
      <input type="hidden" name="account_id" value="${escapeHtml(account.id)}">
      <button type="submit">停用</button></form>`) + '</td></tr>').join('');
  const pendingDeletions = db.prepare(`SELECT id,account_id,requested_at,due_at
    FROM account_deletion_requests WHERE status='pending' ORDER BY due_at`).all();
  const deletionRows = pendingDeletions.map(request => `<tr>
    <td>${escapeHtml(request.id)}</td>
    <td>${escapeHtml(request.account_id)}</td>
    <td>${escapeHtml(new Date(request.due_at).toISOString().slice(0, 10))}</td>
    </tr>`).join('');
  const pendingReports = db.prepare(`SELECT safety_reports.id,safety_reports.reason,
    safety_reports.details,safety_reports.content_excerpt,safety_reports.created_at,
    reporter.account_id AS reporter_id,target.account_id AS target_id
    FROM safety_reports JOIN users reporter ON reporter.id=safety_reports.reporter_user_id
    JOIN users target ON target.id=safety_reports.target_user_id
    WHERE safety_reports.status='pending' ORDER BY safety_reports.created_at LIMIT 100`).all();
  const reportRows = pendingReports.map(report => `<tr>
    <td>${escapeHtml(report.id)}</td>
    <td>${escapeHtml(report.reporter_id)} → ${escapeHtml(report.target_id)}</td>
    <td>${escapeHtml(report.reason)}<br>${escapeHtml(report.details || '')}</td>
    <td>${escapeHtml(report.content_excerpt || '')}</td>
    <td><form method="post" action="/resolve-report">
      <input type="hidden" name="report_id" value="${escapeHtml(report.id)}">
      <input name="resolution" required maxlength="200" placeholder="给举报人的处理说明">
      <button type="submit">标记已处理</button></form></td></tr>`).join('');
  const circleRows = videoCircle?.pendingReview().map(post => `<tr>
    <td>${escapeHtml(post.title)}<br>${escapeHtml(post.author_id)}</td>
    <td>${escapeHtml(post.source_tool)} · ${escapeHtml(post.source_task_id)}<br>
    ${escapeHtml(String(post.duration_ms))} ms / ${escapeHtml(String(post.size_bytes))} B</td>
    <td><a href="/video-circle/preview/${escapeHtml(post.id)}" target="_blank"
      rel="noreferrer">预览视频</a></td>
    <td><form method="post" action="/video-circle/review">
      <input type="hidden" name="post_id" value="${escapeHtml(post.id)}">
      <select name="decision"><option value="approve">批准公开</option>
        <option value="reject">拒绝</option></select>
      <label><input type="checkbox" name="checked" value="yes" required>
        已检查画面、AI 标识、标题与来源</label>
      <input name="reason" maxlength="80" placeholder="拒绝时填写原因">
      <button type="submit">提交复核</button></form></td></tr>`).join('') || '';
  const circlePublishedRows = videoCircle?.publishedReview().map(post => `<tr>
    <td>${escapeHtml(post.title)}<br>${escapeHtml(post.author_id)}</td>
    <td><a href="/video-circle/preview/${escapeHtml(post.id)}" target="_blank"
      rel="noreferrer">预览视频</a></td>
    <td><form method="post" action="/video-circle/hide">
      <input type="hidden" name="post_id" value="${escapeHtml(post.id)}">
      <input name="reason" required maxlength="80" placeholder="下架原因">
      <button type="submit">下架</button></form></td></tr>`).join('') || '';
  const circleReportRows = videoCircle?.pendingReports().map(report => `<tr>
    <td>${escapeHtml(report.title)}<br>${escapeHtml(report.reporter_id)}</td>
    <td>${escapeHtml(report.reason)}<br>${escapeHtml(report.details || '')}</td>
    <td><form method="post" action="/video-circle/resolve-report">
      <input type="hidden" name="report_id" value="${escapeHtml(report.id)}">
      <input name="resolution" required maxlength="80" placeholder="处理结果">
      <button type="submit">标记已处理</button></form></td></tr>`).join('') || '';
  const circleCleanupRows = videoCircle?.pendingCleanup().map(post => `<tr>
    <td>${escapeHtml(post.title)}<br>${escapeHtml(post.author_id)}</td>
    <td>${escapeHtml(post.status)}</td>
    <td><form method="post" action="/video-circle/retry-cleanup">
      <input type="hidden" name="post_id" value="${escapeHtml(post.id)}">
      <button type="submit">重试删除对象</button></form></td></tr>`).join('') || '';
  const circleSection = videoCircle ? `<h2>AI 视频圈 · 待审核</h2>
    <p>仅经 SSH 转发访问；批准前须实际播放并检查内容与 AI 标识。</p>
    <table><tr><th>作品／作者</th><th>来源／规格</th><th>预览</th><th>操作</th></tr>
    ${circleRows || '<tr><td colspan="4">暂无待审核视频</td></tr>'}</table>
    <h2>公开视频</h2><table><tr><th>作品</th><th>预览</th><th>操作</th></tr>
    ${circlePublishedRows || '<tr><td colspan="3">暂无公开视频</td></tr>'}</table>
    <h2>视频举报</h2><table><tr><th>作品／举报人</th><th>原因</th><th>处理</th></tr>
    ${circleReportRows || '<tr><td colspan="3">暂无待处理举报</td></tr>'}</table>
    <h2>待清理视频对象</h2><table><tr><th>作品／作者</th><th>状态</th><th>操作</th></tr>
    ${circleCleanupRows || '<tr><td colspan="3">暂无待清理对象</td></tr>'}</table>` : '';
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
    <h2>待完成的账号注销</h2>
    <p>先按账号注销操作手册清理腾讯 IM、设备同步与素材，再用专用命令和证据文件完成注销。</p>
    <table><tr><th>申请编号</th><th>MaiChat ID</th><th>最迟完成日</th></tr>
    ${deletionRows || '<tr><td colspan="3">暂无待处理申请</td></tr>'}</table>
    <h2>待处理举报</h2><table><tr><th>举报编号</th><th>双方账号</th>
    <th>原因</th><th>内容摘要</th><th>处理</th></tr>
    ${reportRows || '<tr><td colspan="5">暂无待处理举报</td></tr>'}</table>
    ${circleSection}
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
      response.end(page(db, service.videoCircleAdmin));
      return;
    }
    if (request.method === 'GET' && /^\/video-circle\/preview\/[0-9a-f-]{36}$/.test(request.url) &&
        service.videoCircleAdmin) {
      const id = request.url.slice('/video-circle/preview/'.length);
      const preview = await service.videoCircleAdmin.adminPreview(id);
      response.writeHead(preview.statusCode, preview.headers ||
        { 'Content-Type': 'application/json; charset=utf-8' });
      response.end(preview.statusCode === 302 ? '' : JSON.stringify(preview.body));
      return;
    }
    const origin = request.headers.origin;
    const host = request.headers.host;
    let originUrl;
    try { originUrl = new URL(origin); } catch { originUrl = null; }
    if (request.method !== 'POST' ||
        !['/create', '/disable', '/resolve-report', '/video-circle/review',
          '/video-circle/hide', '/video-circle/resolve-report',
          '/video-circle/retry-cleanup'].includes(request.url) ||
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
    let result;
    if (request.url === '/create') {
      result = await service.createTestAccount(accountId, form.get('password'));
    } else if (request.url === '/disable') {
      result = service.disableTestAccount(accountId);
    } else if (request.url === '/resolve-report') {
      const reportId = form.get('report_id');
      const resolution = form.get('resolution')?.trim();
      if (!/^[0-9a-f-]{36}$/.test(reportId || '') || !resolution ||
          Buffer.byteLength(resolution, 'utf8') > 600 ||
          /[\u0000-\u001f\u007f-\u009f]/u.test(resolution)) {
        result = { statusCode: 400, body: { error: 'Invalid report resolution' } };
      } else {
        const changed = db.prepare(`UPDATE safety_reports SET status='resolved',
          reviewed_at=?,resolution=? WHERE id=? AND status='pending'`)
          .run(Date.now(), resolution, reportId).changes;
        result = changed ? { statusCode: 200, body: {} } :
          { statusCode: 404, body: { error: 'Pending report not found' } };
      }
    } else if (!service.videoCircleAdmin) {
      result = { statusCode: 404, body: { error: 'Video circle is unavailable' } };
    } else if (request.url === '/video-circle/review') {
      const decision = form.get('decision');
      const reason = form.get('reason')?.trim() || '';
      result = form.get('checked') === 'yes' ?
        await service.videoCircleAdmin.review(form.get('post_id'), decision, 'admin', reason) :
        { statusCode: 400, body: { error: 'Watch and check the video before review' } };
    } else if (request.url === '/video-circle/hide') {
      result = await service.videoCircleAdmin.hide(form.get('post_id'), 'admin',
        form.get('reason')?.trim() || '');
    } else if (request.url === '/video-circle/retry-cleanup') {
      result = await service.videoCircleAdmin.retryCleanup(form.get('post_id'));
    } else {
      result = service.videoCircleAdmin.resolveReport(form.get('report_id'), 'admin',
        form.get('resolution')?.trim() || '');
    }
    response.writeHead(result.statusCode < 400 ? 200 : result.statusCode,
      { 'Content-Type': 'text/html; charset=utf-8' });
    response.end(page(db, service.videoCircleAdmin,
      result.statusCode < 400 ? '操作成功' : result.body.error));
    } catch {
      if (!response.headersSent) response.writeHead(500);
      response.end('Admin operation failed');
    }
  });
}

module.exports = { createAdminServer };
