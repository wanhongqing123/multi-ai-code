'use strict';

// Public AI videos are metadata here. Media bytes go directly between the App and private OSS.
const { randomUUID } = require('node:crypto');

const MAX_VIDEO_BYTES = 200_000_000;
const MAX_DURATION_MS = 60_000;
const UPLOAD_TTL_MS = 15 * 60_000;
const SOURCE_TOOLS = new Set([
  'wan_video', 'kling_bailian_video', 'minimax_video', 'seedance_video'
]);
const REPORT_REASONS = new Set([
  'spam', 'harassment', 'sexual', 'violence', 'copyright', 'other'
]);

function result(statusCode, body, headers) {
  return { statusCode, body, ...(headers ? { headers } : {}) };
}

function validTitle(value) {
  return typeof value === 'string' && value === value.trim() &&
    [...value].length >= 1 && [...value].length <= 80 &&
    Buffer.byteLength(value, 'utf8') <= 240 &&
    !/[\u0000-\u001f\u007f-\u009f\u2028\u2029]/u.test(value);
}

function validID(value) {
  return typeof value === 'string' && /^[0-9a-f-]{36}$/.test(value);
}

function validObjectKey(accountId, key) {
  return typeof key === 'string' &&
    new RegExp(`^video-circle/${accountId}/\\d{4}-\\d{2}-\\d{2}/` +
      '[0-9a-f-]{36}\\.mp4$').test(key);
}

function encodeCursor(row) {
  return Buffer.from(JSON.stringify([row.published_at, row.id])).toString('base64url');
}

function decodeCursor(value) {
  if (!value) return null;
  if (typeof value !== 'string' || value.length > 160) return false;
  try {
    const decoded = JSON.parse(Buffer.from(value, 'base64url').toString('utf8'));
    return Array.isArray(decoded) && decoded.length === 2 &&
      Number.isSafeInteger(decoded[0]) && decoded[0] > 0 && validID(decoded[1])
      ? decoded : false;
  } catch { return false; }
}

function createVideoCircleService({ db, now = Date.now, verifyUpload,
  signRead, deleteObject, newID = randomUUID }) {
  if (!db) throw new Error('Video circle database is required');

  async function submit(body, principal) {
    const { title, object_key: key, source_tool: sourceTool,
      source_task_id: taskId, size_bytes: bytes, duration_ms: duration } = body;
    if (!validTitle(title) || !validObjectKey(principal.account_id, key) ||
        !SOURCE_TOOLS.has(sourceTool) || typeof taskId !== 'string' ||
        !/^[A-Za-z0-9_-]{1,128}$/.test(taskId) ||
        !Number.isSafeInteger(bytes) || bytes < 1 || bytes > MAX_VIDEO_BYTES ||
        !Number.isSafeInteger(duration) || duration < 1 || duration > MAX_DURATION_MS ||
        body.ai_generated !== true)
      return result(400, { error: 'Invalid AI video submission' });
    if (typeof verifyUpload !== 'function')
      return result(503, { error: 'Video upload verification is unavailable' });
    let verified;
    try { verified = await verifyUpload(principal.account_id, key, bytes); }
    catch { return result(502, { error: 'Could not verify the uploaded video' }); }
    if (verified?.exists !== true || verified.size_bytes !== bytes ||
        verified.content_type !== 'video/mp4')
      return result(400, { error: 'Uploaded video does not match the submission' });
    return db.transaction(() => {
      const author = db.prepare('SELECT disabled_at FROM users WHERE id=?')
        .get(principal.user_id);
      if (!author || author.disabled_at)
        return result(401, { error: 'Account is no longer active' });
      const previous = db.prepare('SELECT id,author_user_id,status FROM video_circle_posts WHERE object_key=?')
        .get(key);
      if (previous) return previous.author_user_id === principal.user_id
        ? result(200, { post_id: previous.id, status: previous.status })
        : result(409, { error: 'Video object was already submitted' });
      const recent = db.prepare(`SELECT COUNT(*) AS count FROM video_circle_posts
        WHERE author_user_id=? AND created_at>?`).get(principal.user_id, now() - 86_400_000);
      const pending = db.prepare(`SELECT COUNT(*) AS count FROM video_circle_posts
        WHERE author_user_id=? AND status='pending'`).get(principal.user_id);
      if (recent.count >= 10 || pending.count >= 10)
        return result(429, { error: 'Too many videos await review' });
      const id = newID();
      db.prepare(`INSERT INTO video_circle_posts
        (id,author_user_id,title,object_key,source_tool,source_task_id,size_bytes,
        content_type,duration_ms,status,created_at)
        VALUES(?,?,?,?,?,?,?,'video/mp4',?,'pending',?)`).run(id, principal.user_id,
        title, key, sourceTool, taskId, bytes, duration, now());
      return result(201, { post_id: id, status: 'pending' });
    })();
  }

  function mine(principal) {
    const rows = db.prepare(`SELECT id,title,source_tool,duration_ms,status,created_at,
      reviewed_at,published_at,withdrawn_at,review_reason FROM video_circle_posts
      WHERE author_user_id=? ORDER BY created_at DESC,id DESC LIMIT 100`)
      .all(principal.user_id);
    return result(200, { posts: rows });
  }

  function feed(principal, before) {
    const cursor = decodeCursor(before);
    if (cursor === false) return result(400, { error: 'Invalid feed cursor' });
    const rows = db.prepare(`SELECT posts.id,posts.title,posts.duration_ms,
      posts.published_at,users.account_id AS author_id,
      COALESCE(NULLIF(users.nickname,''),users.account_id) AS author_name
      FROM video_circle_posts posts JOIN users ON users.id=posts.author_user_id
      WHERE posts.status='approved' AND users.disabled_at IS NULL
        AND NOT EXISTS (SELECT 1 FROM video_circle_blocks blocked
          WHERE blocked.viewer_user_id=? AND blocked.author_user_id=posts.author_user_id)
        AND (? IS NULL OR posts.published_at < ? OR
          (posts.published_at=? AND posts.id<?))
      ORDER BY posts.published_at DESC,posts.id DESC LIMIT 21`)
      .all(principal.user_id, cursor?.[0] ?? null, cursor?.[0] ?? null,
        cursor?.[0] ?? null, cursor?.[1] ?? null);
    const page = rows.slice(0, 20);
    return result(200, { posts: page, next_cursor: rows.length > 20
      ? encodeCursor(page.at(-1)) : null });
  }

  async function play(postId, principal, asJSON = false) {
    if (!validID(postId)) return result(404, { error: 'Video not found' });
    const post = db.prepare(`SELECT posts.id,posts.object_key,posts.status,
      posts.author_user_id,users.disabled_at FROM video_circle_posts posts
      JOIN users ON users.id=posts.author_user_id WHERE posts.id=?`).get(postId);
    const ownPreview = post?.author_user_id === principal.user_id && post?.status === 'pending';
    if (!post || post.disabled_at || (!ownPreview && post.status !== 'approved'))
      return result(404, { error: 'Video not found' });
    if (!ownPreview && db.prepare(`SELECT 1 FROM video_circle_blocks
      WHERE viewer_user_id=? AND author_user_id=?`).get(principal.user_id,post.author_user_id))
      return result(404, { error: 'Video not found' });
    if (typeof signRead !== 'function')
      return result(503, { error: 'Video playback is unavailable' });
    let url;
    try { url = await signRead(post.object_key); } catch { /* keep URL private */ }
    if (typeof url !== 'string' || !url.startsWith('https://'))
      return result(502, { error: 'Could not open the video' });
    const current = db.prepare(`SELECT status FROM video_circle_posts WHERE id=?`).get(postId);
    if (current?.status !== post.status)
      return result(404, { error: 'Video not found' });
    if (asJSON) return result(200, { read_url: url, read_expires_in: 60 });
    return result(302, '', { Location: url, 'Cache-Control': 'no-store' });
  }

  async function withdraw(postId, principal) {
    if (!validID(postId)) return result(404, { error: 'Video not found' });
    const post = db.prepare(`SELECT id,object_key,status,cleanup_pending FROM video_circle_posts
      WHERE id=? AND author_user_id=?`).get(postId, principal.user_id);
    if (!post) return result(404, { error: 'Video not found' });
    if (post.status === 'withdrawn' && !post.cleanup_pending)
      return result(200, { post_id: postId, status: 'withdrawn', cleanup_pending: false });
    if (post.status !== 'withdrawn')
      db.prepare(`UPDATE video_circle_posts SET status='withdrawn',withdrawn_at=?,cleanup_pending=1
        WHERE id=?`).run(now(), postId);
    try {
      if (typeof deleteObject !== 'function') throw new Error('Storage unavailable');
      await deleteObject(post.object_key);
      db.prepare('UPDATE video_circle_posts SET cleanup_pending=0 WHERE id=?').run(postId);
      return result(200, { post_id: postId, status: 'withdrawn', cleanup_pending: false });
    } catch {
      return result(202, { post_id: postId, status: 'withdrawn', cleanup_pending: true });
    }
  }

  function reportVideo(postId, body, principal) {
    if (!validID(postId) || !REPORT_REASONS.has(body.reason) ||
        (body.details !== undefined && (typeof body.details !== 'string' ||
          Buffer.byteLength(body.details, 'utf8') > 600 ||
          /[\u0000-\u001f\u007f-\u009f]/u.test(body.details))))
      return result(400, { error: 'Invalid video report' });
    const post = db.prepare(`SELECT id,author_user_id FROM video_circle_posts
      WHERE id=? AND status='approved'`).get(postId);
    if (!post || post.author_user_id === principal.user_id)
      return result(404, { error: 'Video not found' });
    const existing = db.prepare(`SELECT id,status FROM video_circle_reports
      WHERE post_id=? AND reporter_user_id=?`).get(postId,principal.user_id);
    if (existing) return result(200, { report_id: existing.id, status: existing.status });
    const id = newID();
    db.prepare(`INSERT INTO video_circle_reports
      (id,post_id,reporter_user_id,reason,details,status,created_at)
      VALUES(?,?,?,?,?,'pending',?)`).run(id,postId,principal.user_id,body.reason,
      body.details || null,now());
    return result(201, { report_id: id, status: 'pending' });
  }

  function blockAuthor(body, principal) {
    const accountId = body.author_id;
    if (typeof accountId !== 'string' || !/^[a-z][a-z0-9_-]{2,31}$/.test(accountId) ||
        accountId === principal.account_id)
      return result(400, { error: 'Invalid author' });
    const author = db.prepare('SELECT id FROM users WHERE account_id=? AND disabled_at IS NULL')
      .get(accountId);
    if (!author) return result(404, { error: 'Author not found' });
    db.prepare(`INSERT OR IGNORE INTO video_circle_blocks
      (viewer_user_id,author_user_id,created_at) VALUES(?,?,?)`)
      .run(principal.user_id,author.id,now());
    return result(200, { blocked: true, author_id: accountId });
  }

  function unblockAuthor(body, principal) {
    const author = db.prepare('SELECT id FROM users WHERE account_id=?').get(body.author_id);
    if (!author) return result(404, { error: 'Author not found' });
    db.prepare(`DELETE FROM video_circle_blocks WHERE viewer_user_id=? AND author_user_id=?`)
      .run(principal.user_id,author.id);
    return result(200, { blocked: false, author_id: body.author_id });
  }

  async function handle(method, path, body, principal, query) {
    if (!principal) return result(401, { error: 'Authentication required' });
    if (method === 'POST' && path === '/v1/video-circle/posts')
      return submit(body, principal);
    if (method === 'GET' && path === '/v1/video-circle/mine') return mine(principal);
    if (method === 'GET' && path === '/v1/video-circle/feed')
      return feed(principal, query?.get('before'));
    if (method === 'POST' && path === '/v1/video-circle/blocks')
      return blockAuthor(body, principal);
    if (method === 'POST' && path === '/v1/video-circle/blocks/remove')
      return unblockAuthor(body, principal);
    const match = path.match(/^\/v1\/video-circle\/posts\/([0-9a-f-]{36})\/(play|play-link|withdraw|report)$/);
    if (!match) return result(404, { error: 'Not found' });
    if (match[2] === 'play' && method === 'GET') return play(match[1], principal);
    if (match[2] === 'play-link' && method === 'GET')
      return play(match[1], principal, true);
    if (match[2] === 'withdraw' && method === 'POST')
      return withdraw(match[1], principal);
    if (match[2] === 'report' && method === 'POST')
      return reportVideo(match[1], body, principal);
    return result(404, { error: 'Not found' });
  }

  // Admin calls are reachable only from the separately authenticated local admin server.
  function pendingReview() {
    return db.prepare(`SELECT posts.id,posts.title,posts.object_key,posts.duration_ms,
      posts.size_bytes,posts.source_tool,posts.source_task_id,posts.created_at,
      users.account_id AS author_id FROM video_circle_posts posts
      JOIN users ON users.id=posts.author_user_id WHERE posts.status='pending'
      ORDER BY posts.created_at LIMIT 100`).all();
  }

  function publishedReview() {
    return db.prepare(`SELECT posts.id,posts.title,posts.published_at,
      users.account_id AS author_id FROM video_circle_posts posts
      JOIN users ON users.id=posts.author_user_id WHERE posts.status='approved'
      ORDER BY posts.published_at DESC LIMIT 100`).all();
  }

  function pendingReports() {
    return db.prepare(`SELECT reports.id,reports.post_id,reports.reason,
      reports.details,reports.created_at,posts.title,
      users.account_id AS reporter_id FROM video_circle_reports reports
      JOIN video_circle_posts posts ON posts.id=reports.post_id
      JOIN users ON users.id=reports.reporter_user_id
      WHERE reports.status='pending' ORDER BY reports.created_at LIMIT 100`).all();
  }

  function pendingCleanup() {
    return db.prepare(`SELECT posts.id,posts.title,posts.status,
      users.account_id AS author_id FROM video_circle_posts posts
      JOIN users ON users.id=posts.author_user_id
      WHERE posts.cleanup_pending=1 ORDER BY posts.created_at LIMIT 100`).all();
  }

  async function adminPreview(postId) {
    if (!validID(postId)) return result(404, { error: 'Video not found' });
    const post = db.prepare(`SELECT object_key FROM video_circle_posts
      WHERE id=? AND status IN ('pending','approved')`).get(postId);
    if (!post) return result(404, { error: 'Video not found' });
    if (typeof signRead !== 'function')
      return result(503, { error: 'Video playback is unavailable' });
    let url;
    try { url = await signRead(post.object_key); } catch { /* keep storage URL private */ }
    if (typeof url !== 'string' || !url.startsWith('https://'))
      return result(502, { error: 'Could not open the video' });
    return result(302, '', { Location: url, 'Cache-Control': 'no-store' });
  }

  async function review(postId, decision, reviewer, reason = '') {
    if (!validID(postId) || !['approve','reject'].includes(decision) ||
        typeof reviewer !== 'string' || !reviewer || !validTitle(reason || 'Reviewed'))
      return result(400, { error: 'Invalid review decision' });
    if (decision === 'reject' && !reason)
      return result(400, { error: 'A rejection reason is required' });
    const status = decision === 'approve' ? 'approved' : 'rejected';
    const pending = db.prepare(`SELECT posts.object_key,posts.size_bytes,posts.created_at,
      users.account_id FROM video_circle_posts posts
      JOIN users ON users.id=posts.author_user_id
      WHERE posts.id=? AND posts.status='pending' AND users.disabled_at IS NULL`).get(postId);
    if (!pending) return result(404, { error: 'Pending video not found' });
    if (status === 'approved') {
      // The original PUT URL can be replayed until it expires. Never publish before then.
      if (now() < pending.created_at + UPLOAD_TTL_MS)
        return result(409, { error: 'Wait for the upload link to expire before review' });
      if (typeof verifyUpload !== 'function')
        return result(503, { error: 'Video verification is unavailable' });
      let verified;
      try {
        verified = await verifyUpload(pending.account_id,pending.object_key,pending.size_bytes);
      } catch { return result(502, { error: 'Could not verify the video for review' }); }
      if (verified?.exists !== true || verified.size_bytes !== pending.size_bytes ||
          verified.content_type !== 'video/mp4')
        return result(409, { error: 'Reviewed video is no longer available' });
    }
    const timestamp = now();
    const changed = db.prepare(`UPDATE video_circle_posts SET status=?,reviewed_at=?,
      published_at=?,reviewer=?,review_reason=?,cleanup_pending=?
      WHERE id=? AND status='pending'`)
      .run(status,timestamp,status === 'approved' ? timestamp : null,
        reviewer,reason || null,status === 'rejected' ? 1 : 0,postId).changes;
    if (!changed) return result(404,{error:'Pending video not found'});
    if (status === 'approved') return result(200,{post_id:postId,status});
    try {
      if (typeof deleteObject !== 'function') throw new Error('Storage unavailable');
      await deleteObject(pending.object_key);
      db.prepare('UPDATE video_circle_posts SET cleanup_pending=0 WHERE id=?').run(postId);
      return result(200,{post_id:postId,status,cleanup_pending:false});
    } catch { return result(202,{post_id:postId,status,cleanup_pending:true}); }
  }

  async function hide(postId, reviewer, reason) {
    if (!validID(postId) || typeof reviewer !== 'string' || !reviewer ||
        !validTitle(reason)) return result(400, { error: 'Invalid hide decision' });
    const post = db.prepare(`SELECT object_key FROM video_circle_posts
      WHERE id=? AND status='approved'`).get(postId);
    if (!post) return result(404, { error: 'Published video not found' });
    db.prepare(`UPDATE video_circle_posts SET status='hidden',reviewed_at=?,
      reviewer=?,review_reason=?,cleanup_pending=1 WHERE id=?`)
      .run(now(),reviewer,reason,postId);
    try {
      if (typeof deleteObject !== 'function') throw new Error('Storage unavailable');
      await deleteObject(post.object_key);
      db.prepare('UPDATE video_circle_posts SET cleanup_pending=0 WHERE id=?').run(postId);
      return result(200, { post_id: postId, status: 'hidden', cleanup_pending: false });
    } catch {
      return result(202, { post_id: postId, status: 'hidden', cleanup_pending: true });
    }
  }

  function resolveReport(reportId, reviewer, resolution) {
    if (!validID(reportId) || typeof reviewer !== 'string' || !reviewer ||
        !validTitle(resolution)) return result(400, { error: 'Invalid report resolution' });
    const changed = db.prepare(`UPDATE video_circle_reports SET status='resolved',
      reviewed_at=?,reviewer=?,resolution=? WHERE id=? AND status='pending'`)
      .run(now(),reviewer,resolution,reportId).changes;
    return changed ? result(200, { report_id: reportId, status: 'resolved' }) :
      result(404, { error: 'Pending report not found' });
  }

  async function cleanupAccount(userId) {
    const posts = db.prepare(`SELECT id,object_key FROM video_circle_posts
      WHERE author_user_id=? AND cleanup_pending=1`).all(userId);
    let cleaned = 0;
    for (const post of posts) {
      try {
        if (typeof deleteObject !== 'function') break;
        await deleteObject(post.object_key);
        db.prepare('UPDATE video_circle_posts SET cleanup_pending=0 WHERE id=?').run(post.id);
        cleaned++;
      } catch { /* keep this object in the operator cleanup queue */ }
    }
    return { cleaned, pending: posts.length - cleaned };
  }

  async function retryCleanup(postId) {
    if (!validID(postId)) return result(404, { error: 'Video not found' });
    const post = db.prepare(`SELECT object_key FROM video_circle_posts
      WHERE id=? AND cleanup_pending=1 AND status IN ('withdrawn','hidden','rejected')`)
      .get(postId);
    if (!post) return result(404, { error: 'Pending cleanup not found' });
    try {
      if (typeof deleteObject !== 'function') throw new Error('Storage unavailable');
      await deleteObject(post.object_key);
      db.prepare('UPDATE video_circle_posts SET cleanup_pending=0 WHERE id=?').run(postId);
      return result(200, { post_id: postId, cleanup_pending: false });
    } catch { return result(502, { error: 'Video object cleanup failed' }); }
  }

  return { handle, pendingReview, publishedReview, pendingReports, pendingCleanup,
    adminPreview, review, hide, resolveReport, cleanupAccount, retryCleanup };
}

module.exports = { createVideoCircleService };
