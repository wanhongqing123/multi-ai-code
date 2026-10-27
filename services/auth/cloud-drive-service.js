'use strict';

// Account-bound metadata and resumable OSS multipart control. Media bytes never enter this service.
const { randomUUID } = require('node:crypto');

const DEFAULT_VIDEO_BYTES = 2_000_000_000;
// OSS on-the-fly image processing accepts source images up to 20 MB.
const DEFAULT_IMAGE_BYTES = 20_000_000;
const DEFAULT_QUOTA_BYTES = 10_000_000_000;
const PART_BYTES = 8 * 1024 * 1024;
const UPLOAD_TTL_MS = 7 * 24 * 60 * 60_000;
const COMPLETION_HOLD_MS = 24 * 60 * 60_000;
const CLEANUP_RETRY_MS = 60 * 60_000;
const PAGE_SIZE = 50;

function result(statusCode, body) { return { statusCode, body }; }

function validID(value) {
  return typeof value === 'string' &&
    /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/.test(value);
}

function validUploadID(value) {
  return typeof value === 'string' && /^[A-Za-z0-9_+\/=-]{1,512}$/.test(value);
}

function mediaType(name, mode = 'drive') {
  if (typeof name !== 'string' || name !== name.trim() ||
      Buffer.byteLength(name,'utf8') > 240 || [...name].length < 1 ||
      /[\u0000-\u001f\u007f-\u009f\u2028\u2029/\\]/u.test(name)) return null;
  if (mode === 'session' && (/[<>:"|?*]/u.test(name) || /[. ]$/u.test(name) ||
      /^(con|prn|aux|nul|com[1-9]|lpt[1-9])(?:\.|$)/i.test(name))) return null;
  if (/\.mp4$/i.test(name)) return { contentType:'video/mp4', extension:'.mp4',kind:'video' };
  if (/\.mov$/i.test(name)) return { contentType:'video/quicktime', extension:'.mov',kind:'video' };
  if (/\.jpe?g$/i.test(name))
    return { contentType:'image/jpeg',extension:/\.jpeg$/i.test(name) ? '.jpeg' : '.jpg',kind:'image' };
  if (/\.png$/i.test(name)) return { contentType:'image/png',extension:'.png',kind:'image' };
  if (/\.webp$/i.test(name)) return { contentType:'image/webp',extension:'.webp',kind:'image' };
  if (/\.heic$/i.test(name)) return { contentType:'image/heic',extension:'.heic',kind:'image' };
  if (mode === 'session') {
    if (/\.gif$/i.test(name))
      return { contentType:'image/gif',extension:'.gif',kind:'image' };
    const audio = { mp3:'audio/mpeg', m4a:'audio/mp4', aac:'audio/aac',
      wav:'audio/wav', flac:'audio/flac', ogg:'audio/ogg' };
    const suffix = name.match(/\.([a-zA-Z0-9]{1,12})$/);
    if (suffix) {
      const extension = '.' + suffix[1].toLowerCase();
      return { contentType:audio[suffix[1].toLowerCase()] ||
        'application/octet-stream',extension,
        kind:audio[suffix[1].toLowerCase()] ? 'audio' : 'file' };
    }
  }
  return null;
}

function encodeCursor(row) {
  return Buffer.from(JSON.stringify([row.created_at,row.id])).toString('base64url');
}

function decodeCursor(value) {
  if (!value) return null;
  if (typeof value !== 'string' || value.length > 160) return false;
  try {
    const pair = JSON.parse(Buffer.from(value,'base64url').toString('utf8'));
    return Array.isArray(pair) && pair.length === 2 &&
      Number.isSafeInteger(pair[0]) && pair[0] > 0 && validID(pair[1])
      ? pair : false;
  } catch { return false; }
}

function validMD5(value) {
  return typeof value === 'string' && /^[A-Za-z0-9+/]{22}==$/.test(value) &&
    Buffer.from(value,'base64').length === 16 &&
    Buffer.from(value,'base64').toString('base64') === value;
}

function createCloudDriveService({ db, now = Date.now, startUpload, signPart,
  listParts, listUploads, completeUpload, abortUpload, verifyObject, signRead,
  signThumbnail, deleteObject,
  newID = randomUUID, maxVideoBytes = DEFAULT_VIDEO_BYTES,
  maxImageBytes = DEFAULT_IMAGE_BYTES,
  quotaBytes = DEFAULT_QUOTA_BYTES, allowedAccountIds = null,
  mode = 'drive' }) {
  if (!db || !Number.isSafeInteger(maxVideoBytes) || maxVideoBytes < PART_BYTES ||
      !Number.isSafeInteger(maxImageBytes) || maxImageBytes < 1 ||
      maxImageBytes > DEFAULT_IMAGE_BYTES ||
      !Number.isSafeInteger(quotaBytes) || quotaBytes < maxVideoBytes ||
      (allowedAccountIds !== null && !(allowedAccountIds instanceof Set)) ||
      !['drive','session'].includes(mode))
    throw new Error('Invalid cloud drive configuration');

  const table = mode === 'session' ? 'session_attachments' : 'cloud_drive_videos';
  const root = mode === 'session' ? '/v1/session-attachments' :
    '/v1/cloud-drive/videos';
  // The table name comes only from the two constants above, never from a request.
  const prepare = sql => db.prepare(sql.replaceAll('cloud_drive_videos',table));
  const roomKey = body => {
    const slot = body?.owner_slot;
    const login = body?.owner_login_session_id;
    const session = body?.session_id;
    if (!['ios_phone','android_phone','tablet','mac','windows'].includes(slot) ||
        !/^[A-Za-z0-9_-]{1,128}$/.test(login || '') ||
        !/^[A-Za-z0-9_-]{1,128}$/.test(session || '')) return null;
    // Session history survives a new login on the same owner slot.
    return `${slot}:${session}`;
  };

  const storageReady = (...callbacks) => callbacks.every(callback =>
    typeof callback === 'function');
  const expiresISO = value => new Date(value).toISOString();
  const partsTotal = row => Math.ceil(row.size_bytes/PART_BYTES);
  const partBytes = (row,partNumber) => Math.min(PART_BYTES,
    row.size_bytes-(partNumber-1)*PART_BYTES);
  const own = (id,userId) => prepare(`SELECT * FROM cloud_drive_videos
    WHERE id=? AND owner_user_id=?`).get(id,userId);

  function quota(owner) {
    const usage = prepare(`SELECT
      COALESCE(SUM(CASE WHEN status='ready' THEN size_bytes ELSE 0 END),0) AS used_bytes,
      COALESCE(SUM(CASE WHEN status IN ('uploading','completing') OR cleanup_pending=1
        THEN size_bytes ELSE 0 END),0) AS reserved_bytes
      FROM cloud_drive_videos WHERE owner_user_id=?`).get(owner);
    return { ...usage, limit_bytes:quotaBytes };
  }

  function publicReady(row) {
    return { video_id:row.id,status:'ready',name:row.name,
      size_bytes:row.size_bytes,content_type:row.content_type,
      media_type:row.content_type.startsWith('image/') ? 'image' :
        row.content_type.startsWith('audio/') ? 'audio' :
        row.content_type.startsWith('video/') ? 'video' : 'file',
      ...(mode === 'session' ? { attachment_id:row.id,room_key:row.room_key } : {}),
      created_at:row.created_at };
  }

  async function settleDeletedCompletion(row,principal) {
    const timestamp = now();
    const changed = prepare(`UPDATE cloud_drive_videos SET cleanup_pending=1,
      cleanup_after=?,cleanup_hold_until=MAX(COALESCE(cleanup_hold_until,0),?),
      cleanup_revision=cleanup_revision+1
      WHERE id=? AND owner_user_id=? AND status='deleted'`)
      .run(timestamp,timestamp+COMPLETION_HOLD_MS,row.id,principal.user_id).changes;
    if (changed) {
      await sweep();
      return result(404,{ error:'Video not found' });
    }
    const current = own(row.id,principal.user_id);
    if (current?.status === 'ready') return result(200,publicReady(current));
    // A completed account purge can remove the row after its hold horizon.
    // Still attempt to delete any late object observed by this running handler.
    try {
      if (typeof deleteObject === 'function')
        await deleteObject(principal.account_id,row.object_key);
    } catch { /* operator OSS lifecycle/prefix purge remains the final fallback */ }
    return result(404,{ error:'Video not found' });
  }

  async function begin(body,principal) {
    const media = mediaType(body.name,mode);
    const bytes = body.size_bytes;
    const binding = mode === 'session' ? roomKey(body) : null;
    if (mode === 'session' && !binding)
      return result(400,{ error:'Invalid Session binding' });
    if (!media || body.content_type !== media.contentType ||
        !Number.isSafeInteger(bytes) || bytes < 1 ||
        bytes > (media.kind === 'image' ? maxImageBytes : maxVideoBytes))
      return result(400,{ error:'Invalid cloud media upload' });
    if (!storageReady(startUpload,abortUpload))
      return result(503,{ error:'Cloud video upload is unavailable' });
    const id = newID();
    if (!validID(id)) throw new Error('Invalid cloud video ID');
    const created = now();
    const key = `${mode === 'session' ? 'session-attachments' : 'cloud-drive'}/${principal.account_id}/` +
      `${new Date(created).toISOString().slice(0,10)}/${id}${media.extension}`;
    const expires = created+UPLOAD_TTL_MS;
    const reservation = db.transaction(() => {
      if (!prepare('SELECT 1 FROM users WHERE id=? AND disabled_at IS NULL')
        .get(principal.user_id))
        return result(401,{ error:'Account is no longer active' });
      const available = quota(principal.user_id);
      if (available.used_bytes+available.reserved_bytes+bytes > quotaBytes)
        return result(413,{ error:'Cloud drive quota exceeded',quota:available });
      const pending = prepare(`SELECT COUNT(*) AS count FROM cloud_drive_videos
        WHERE owner_user_id=? AND status IN ('uploading','completing')`)
        .get(principal.user_id).count;
      if (pending >= 10)
        return result(429,{ error:'Too many unfinished uploads' });
      prepare(`INSERT INTO cloud_drive_videos
        (id,owner_user_id,name,object_key,size_bytes,content_type,status,
        created_at,upload_expires_at${mode === 'session' ? ',room_key' : ''})
        VALUES(?,?,?,?,?,?,'uploading',?,?${mode === 'session' ? ',?' : ''})`)
        .run(id,principal.user_id,body.name,key,bytes,media.contentType,created,
          expires,...(mode === 'session' ? [binding] : []));
      return null;
    })();
    if (reservation) return reservation;
    let uploadId;
    try { uploadId = await startUpload(principal.account_id,key,media.contentType); }
    catch { /* reservation is released below */ }
    if (!validUploadID(uploadId)) {
      // The OSS init request may have succeeded even if its response was lost.
      // Keep the exact key until ListMultipartUploads can prove it is empty.
      prepare(`UPDATE cloud_drive_videos SET status='deleted',deleted_at=?,
        cleanup_pending=1,cleanup_after=?,cleanup_hold_until=?,
        cleanup_revision=cleanup_revision+1
        WHERE id=? AND owner_user_id=? AND status='uploading'
        AND upload_id IS NULL`)
        .run(now(),now(),now()+COMPLETION_HOLD_MS,id,principal.user_id);
      await sweep();
      return result(502,{ error:'Could not begin the video upload' });
    }
    const saved = prepare(`UPDATE cloud_drive_videos SET upload_id=?
      WHERE id=? AND owner_user_id=? AND status='uploading' AND upload_id IS NULL`)
      .run(uploadId,id,principal.user_id).changes;
    if (!saved) {
      // Deletion may have happened while OSS was creating the multipart task.
      // Keep its returned upload ID in the tombstone so abort can be retried.
      const retained = prepare(`UPDATE cloud_drive_videos SET upload_id=?,
        cleanup_pending=1,cleanup_after=?,
        cleanup_hold_until=MAX(COALESCE(cleanup_hold_until,0),?),
        cleanup_revision=cleanup_revision+1
        WHERE id=? AND owner_user_id=? AND status='deleted' AND upload_id IS NULL`)
        .run(uploadId,now(),now()+COMPLETION_HOLD_MS,id,principal.user_id).changes;
      if (retained) await sweep();
      else {
        try { await abortUpload(principal.account_id,key,uploadId); }
        catch { /* OSS lifecycle fallback after a completed tombstone */ }
      }
      return result(409,{ error:'Upload was canceled' });
    }
    return result(201,{ video_id:id,upload_id:uploadId,media_type:media.kind,
      ...(mode === 'session' ? { attachment_id:id,room_key:binding } : {}),
      part_size:PART_BYTES,
      parts_total:Math.ceil(bytes/PART_BYTES),upload_expires_at:expiresISO(expires),
      quota:quota(principal.user_id) });
  }

  function uploadRow(id,principal) {
    if (!validID(id)) return result(404,{ error:'Video not found' });
    const row = own(id,principal.user_id);
    if (!row || row.status === 'deleted')
      return result(404,{ error:'Video not found' });
    if (row.status === 'ready') return result(409,{ error:'Upload already completed' });
    if (row.upload_expires_at <= now())
      return result(410,{ error:'Upload expired' });
    if (!row.upload_id)
      return result(409,{ error:'Upload is starting' });
    return row;
  }

  async function signOne(id,number,body,principal) {
    const row = uploadRow(id,principal);
    if (row.statusCode) return row;
    if (row.status !== 'uploading')
      return result(409,{ error:'Upload is completing' });
    const partNumber = Number(number);
    if (!/^[1-9][0-9]{0,3}$/.test(number) ||
        !Number.isInteger(partNumber) || partNumber > partsTotal(row) ||
        !validMD5(body.content_md5_base64))
      return result(400,{ error:'Invalid upload part' });
    if (!storageReady(signPart))
      return result(503,{ error:'Part signing is unavailable' });
    const expected = partBytes(row,partNumber);
    let signed;
    try { signed = await signPart(principal.account_id,row.object_key,row.upload_id,
      partNumber,expected,body.content_md5_base64); }
    catch { return result(502,{ error:'Could not sign upload part' }); }
    if (typeof signed?.upload_url !== 'string' ||
        !signed.upload_url.startsWith('https://') ||
        signed.upload_headers?.['Content-Length'] !== String(expected) ||
        signed.upload_headers?.['Content-MD5'] !== body.content_md5_base64)
      return result(502,{ error:'Invalid signed upload part' });
    const current = own(id,principal.user_id);
    if (current?.status !== 'uploading' || current.upload_id !== row.upload_id)
      return result(409,{ error:'Upload changed while signing' });
    prepare('UPDATE cloud_drive_videos SET last_part_signed_at=? WHERE id=?')
      .run(now(),id);
    return result(200,{ part_number:partNumber,part_size_bytes:expected,
      upload_url:signed.upload_url,upload_headers:signed.upload_headers,expires_in:300 });
  }

  async function uploadedParts(id,principal) {
    if (validID(id)) {
      const completed = own(id,principal.user_id);
      if (completed?.status === 'ready')
        return result(200,{ video_id:id,status:'ready',part_size:PART_BYTES,
          parts_total:partsTotal(completed),parts:[],
          upload_expires_at:expiresISO(completed.upload_expires_at) });
    }
    const row = uploadRow(id,principal);
    if (row.statusCode) return row;
    if (!storageReady(listParts))
      return result(503,{ error:'Upload resume is unavailable' });
    let parts;
    try { parts = await listParts(principal.account_id,row.object_key,row.upload_id); }
    catch { return result(502,{ error:'Could not list uploaded parts' }); }
    if (!Array.isArray(parts) || parts.some(part =>
        !Number.isInteger(part.part_number) || part.part_number < 1 ||
        !Number.isSafeInteger(part.size_bytes) || part.size_bytes < 1 ||
        typeof part.etag !== 'string' || part.etag.length < 1))
      return result(502,{ error:'Invalid uploaded part list' });
    const valid = parts.filter(part => part.part_number <= partsTotal(row) &&
      part.size_bytes === partBytes(row,part.part_number));
    return result(200,{ video_id:id,status:row.status,part_size:PART_BYTES,
      parts_total:partsTotal(row),parts:valid,
      upload_expires_at:expiresISO(row.upload_expires_at) });
  }

  async function complete(id,principal) {
    if (!validID(id)) return result(404,{ error:'Video not found' });
    const row = own(id,principal.user_id);
    if (!row || row.status === 'deleted')
      return result(404,{ error:'Video not found' });
    if (row.status === 'ready') return result(200,publicReady(row));
    if (row.upload_expires_at <= now())
      return result(410,{ error:'Upload expired' });
    if (!row.upload_id || !storageReady(listParts,completeUpload,verifyObject))
      return result(503,{ error:'Video completion is unavailable' });
    const claimed = prepare(`UPDATE cloud_drive_videos SET status='completing',
      completion_started_at=?
      WHERE id=? AND owner_user_id=? AND status='uploading'`)
      .run(now(),id,principal.user_id).changes;
    if (!claimed) {
      const currentRow = own(id,principal.user_id);
      if (currentRow?.status === 'ready') return result(200,publicReady(currentRow));
      if (currentRow?.status === 'deleted')
        return settleDeletedCompletion(row,principal);
      if (currentRow?.status !== 'completing')
        return result(404,{ error:'Video not found' });
      // A previous completion can have succeeded even if its HTTP reply was lost.
      let head;
      try { head = await verifyObject(principal.account_id,row.object_key,
        row.size_bytes,row.content_type); } catch { /* retry later */ }
      if (head?.exists === true && head.size_bytes === row.size_bytes &&
          head.content_type === row.content_type) {
        const changed = prepare(`UPDATE cloud_drive_videos SET status='ready',ready_at=?
          WHERE id=? AND owner_user_id=? AND status='completing'`)
          .run(now(),id,principal.user_id).changes;
        return changed ? result(200,publicReady(row)) :
          settleDeletedCompletion(row,principal);
      }
      if (now()-(currentRow.completion_started_at || 0) > 150_000) {
        try {
          await listParts(principal.account_id,row.object_key,row.upload_id);
          prepare(`UPDATE cloud_drive_videos SET status='uploading',
            completion_started_at=NULL WHERE id=? AND owner_user_id=?
            AND status='completing' AND completion_started_at=?`)
            .run(id,principal.user_id,currentRow.completion_started_at);
        } catch { /* an OSS completion may still be settling */ }
      }
      if (own(id,principal.user_id)?.status === 'deleted')
        return settleDeletedCompletion(row,principal);
      return result(409,{ error:'Upload completion is in progress' });
    }
    let parts;
    try { parts = await listParts(principal.account_id,row.object_key,row.upload_id); }
    catch {
      prepare(`UPDATE cloud_drive_videos SET status='uploading'
        ,completion_started_at=NULL WHERE id=? AND status='completing'`).run(id);
      if (own(id,principal.user_id)?.status === 'deleted')
        return settleDeletedCompletion(row,principal);
      return result(502,{ error:'Could not verify uploaded parts' });
    }
    const expected = partsTotal(row);
    if (!Array.isArray(parts) || parts.length !== expected ||
        parts.some((part,index) => part.part_number !== index+1 ||
          part.size_bytes !== partBytes(row,index+1) ||
          typeof part.etag !== 'string' || !part.etag)) {
      prepare(`UPDATE cloud_drive_videos SET status='uploading'
        ,completion_started_at=NULL WHERE id=? AND status='completing'`).run(id);
      if (own(id,principal.user_id)?.status === 'deleted')
        return settleDeletedCompletion(row,principal);
      return result(409,{ error:'Video parts are incomplete' });
    }
    try {
      await completeUpload(principal.account_id,row.object_key,row.upload_id,parts);
    } catch {
      // Keep "completing": HEAD on the next call settles an uncertain OSS outcome.
      if (own(id,principal.user_id)?.status === 'deleted')
        return settleDeletedCompletion(row,principal);
      return result(502,{ error:'Could not finish the video upload; retry completion' });
    }
    let head;
    try { head = await verifyObject(principal.account_id,row.object_key,
      row.size_bytes,row.content_type); }
    catch {
      if (own(id,principal.user_id)?.status === 'deleted')
        return settleDeletedCompletion(row,principal);
      return result(502,{ error:'Could not verify completed video; retry completion' });
    }
    if (head?.exists !== true || head.size_bytes !== row.size_bytes ||
        head.content_type !== row.content_type) {
      if (own(id,principal.user_id)?.status === 'deleted')
        return settleDeletedCompletion(row,principal);
      prepare(`UPDATE cloud_drive_videos SET status='deleted',deleted_at=?,
        cleanup_pending=1,cleanup_after=?,cleanup_hold_until=?,
        cleanup_revision=cleanup_revision+1 WHERE id=? AND status='completing'`)
        .run(now(),now(),now()+COMPLETION_HOLD_MS,id);
      await sweep();
      return result(502,{ error:'Completed video size or type mismatch' });
    }
    const changed = prepare(`UPDATE cloud_drive_videos SET status='ready',ready_at=?
      WHERE id=? AND owner_user_id=? AND status='completing'`)
      .run(now(),id,principal.user_id).changes;
    if (!changed) {
      return settleDeletedCompletion(row,principal);
    }
    return result(200,publicReady(row));
  }

  function list(principal,before) {
    const cursor = decodeCursor(before);
    if (cursor === false) return result(400,{ error:'Invalid video cursor' });
    const rows = prepare(`SELECT id,name,size_bytes,content_type,created_at${mode === 'session' ? ',room_key' : ''}
      FROM cloud_drive_videos WHERE owner_user_id=? AND status='ready'
      AND (? IS NULL OR created_at<? OR (created_at=? AND id<?))
      ORDER BY created_at DESC,id DESC LIMIT ?`)
      .all(principal.user_id,cursor?.[0] ?? null,cursor?.[0] ?? null,
        cursor?.[0] ?? null,cursor?.[1] ?? null,PAGE_SIZE+1);
    const page = rows.slice(0,PAGE_SIZE).map(row => ({ ...row,
      media_type:row.content_type.startsWith('image/') ? 'image' :
        row.content_type.startsWith('audio/') ? 'audio' :
        row.content_type.startsWith('video/') ? 'video' : 'file' }));
    return result(200,{ videos:page,
      next_cursor:rows.length > PAGE_SIZE ? encodeCursor(page.at(-1)) : null,
      quota:quota(principal.user_id) });
  }

  async function play(id,principal) {
    if (!validID(id)) return result(404,{ error:'Video not found' });
    const row = own(id,principal.user_id);
    if (!row || row.status !== 'ready')
      return result(404,{ error:'Video not found' });
    if (!storageReady(signRead))
      return result(503,{ error:'Video playback is unavailable' });
    let url;
    try { url = await signRead(principal.account_id,row.object_key); }
    catch { /* storage detail is private */ }
    if (typeof url !== 'string' || !url.startsWith('https://'))
      return result(502,{ error:'Could not open the video' });
    if (own(id,principal.user_id)?.status !== 'ready')
      return result(404,{ error:'Video not found' });
    return result(200,{ read_url:url,read_expires_in:60,
      size_bytes:row.size_bytes,content_type:row.content_type,
      media_type:row.content_type.startsWith('image/') ? 'image' :
        row.content_type.startsWith('audio/') ? 'audio' :
        row.content_type.startsWith('video/') ? 'video' : 'file',
      ...(mode === 'session' ? { attachment_id:row.id,name:row.name,
        room_key:row.room_key } : {}) });
  }

  async function thumbnail(id,principal) {
    if (!validID(id)) return result(404,{ error:'Media not found' });
    const row = own(id,principal.user_id);
    if (!row || row.status !== 'ready')
      return result(404,{ error:'Media not found' });
    if (!storageReady(signThumbnail))
      return result(503,{ error:'Thumbnail is unavailable' });
    let url;
    try { url = await signThumbnail(principal.account_id,row.object_key); }
    catch { /* OSS processing errors are handled by the client placeholder. */ }
    if (typeof url !== 'string' || !url.startsWith('https://'))
      return result(502,{ error:'Could not open thumbnail' });
    if (own(id,principal.user_id)?.status !== 'ready')
      return result(404,{ error:'Media not found' });
    return result(200,{ thumbnail_url:url,thumbnail_expires_in:60,
      content_type:'image/jpeg' });
  }

  async function remove(id,principal) {
    if (!validID(id)) return result(404,{ error:'Video not found' });
    const row = own(id,principal.user_id);
    if (!row) return result(404,{ error:'Video not found' });
    if (row.status !== 'deleted')
      prepare(`UPDATE cloud_drive_videos SET status='deleted',deleted_at=?,
        cleanup_pending=1,cleanup_after=?,
        cleanup_hold_until=CASE WHEN status='completing' OR
          (status='uploading' AND upload_id IS NULL) THEN ? ELSE NULL END,
        cleanup_revision=cleanup_revision+1
        WHERE id=? AND owner_user_id=?`)
        .run(now(),now(),now()+COMPLETION_HOLD_MS,id,principal.user_id);
    await sweep();
    const after = own(id,principal.user_id);
    return result(after?.cleanup_pending ? 202 : 200,
      { video_id:id,status:'deleted',cleanup_pending:Boolean(after?.cleanup_pending),
        quota:quota(principal.user_id) });
  }

  async function sweep() {
    const current = now();
    prepare(`UPDATE cloud_drive_videos SET status='deleted',deleted_at=?,
      cleanup_pending=1,cleanup_after=?,
      cleanup_hold_until=CASE WHEN status='completing' OR
        (status='uploading' AND upload_id IS NULL) THEN ? ELSE NULL END,
      cleanup_revision=cleanup_revision+1
      WHERE status IN ('uploading','completing') AND upload_expires_at<=?`)
      .run(current,current,current+COMPLETION_HOLD_MS,current);
    // Old tombstones can predate the initialization-race hold. Do not discard
    // one while an OSS init response may still be returning an upload ID.
    prepare(`UPDATE cloud_drive_videos SET cleanup_hold_until=?,
      cleanup_revision=cleanup_revision+1
      WHERE status='deleted' AND cleanup_pending=1 AND upload_id IS NULL
      AND cleanup_hold_until IS NULL`)
      .run(current+COMPLETION_HOLD_MS);
    const fields = `id,owner_user_id,object_key,upload_id,size_bytes,
      content_type,deleted_at,cleanup_pending,cleanup_hold_until,
      cleanup_revision`;
    // Reserve work for both queues so repeatedly failing pending rows cannot
    // starve pre-upgrade cleanup-complete tombstones forever.
    const rows = [
      ...prepare(`SELECT ${fields} FROM cloud_drive_videos
        WHERE status='deleted' AND cleanup_pending=1
        AND COALESCE(cleanup_after,0)<=?
        ORDER BY COALESCE(cleanup_after,0),id LIMIT 50`).all(current),
      ...prepare(`SELECT ${fields} FROM cloud_drive_videos
        WHERE status='deleted' AND cleanup_pending=0
        ORDER BY COALESCE(deleted_at,0),id LIMIT 50`).all()
    ];
    let cleaned = 0;
    for (const row of rows) {
      const legacy = row.cleanup_pending === 0;
      const holdUntil = Math.max(row.cleanup_hold_until || 0,
        legacy ? (row.deleted_at || current)+COMPLETION_HOLD_MS : 0);
      const requeueLegacy = nextAt => prepare(`UPDATE cloud_drive_videos
        SET cleanup_pending=1,cleanup_after=?,cleanup_hold_until=?,
        cleanup_revision=cleanup_revision+1 WHERE id=? AND status='deleted'
        AND cleanup_pending=0 AND cleanup_revision=?`)
        .run(nextAt,holdUntil,row.id,row.cleanup_revision).changes;
      try {
        const owner = prepare('SELECT account_id FROM users WHERE id=?')
          .get(row.owner_user_id)?.account_id;
        if (!owner || !storageReady(listUploads,abortUpload,deleteObject,
            verifyObject)) throw new Error('Cloud cleanup is unavailable');
        if (row.upload_id) await abortUpload(owner,row.object_key,row.upload_id);
        const active = await listUploads(owner,row.object_key);
        if (!Array.isArray(active) || active.some(id => !validUploadID(id)) ||
            new Set(active).size !== active.length)
          throw new Error('Invalid active upload list');
        for (const uploadId of active)
          await abortUpload(owner,row.object_key,uploadId);
        const remaining = await listUploads(owner,row.object_key);
        if (!Array.isArray(remaining) || remaining.length !== 0)
          throw new Error('Multipart upload is still active');
        await deleteObject(owner,row.object_key);
        const head = await verifyObject(owner,row.object_key,
          row.size_bytes,row.content_type);
        if (head?.exists !== false) throw new Error('Deleted video is still present');
        if (holdUntil && current < holdUntil) {
          const nextAt = Math.min(current+CLEANUP_RETRY_MS,holdUntil);
          if (legacy) requeueLegacy(nextAt);
          else prepare(`UPDATE cloud_drive_videos SET cleanup_after=?
            WHERE id=? AND status='deleted' AND cleanup_pending=1
            AND cleanup_revision=?`)
            .run(nextAt,row.id,row.cleanup_revision);
          continue;
        }
        // Every active exact-key upload was aborted and a second list found
        // none. DELETE succeeded and HEAD confirmed the object is absent.
        // The revision guard protects a tombstone refreshed by a late init or
        // Complete while these network requests were in flight.
        const done = prepare(`DELETE FROM cloud_drive_videos
          WHERE id=? AND status='deleted' AND cleanup_pending=?
          AND cleanup_revision=? AND
          (cleanup_hold_until IS NULL OR cleanup_hold_until<=?)`)
          .run(row.id,row.cleanup_pending,row.cleanup_revision,current).changes;
        cleaned += done;
      } catch {
        // Old cleanup-complete rows did not reserve quota. Reclaim it only if
        // OSS absence could not be proved; successful reconciliation removes
        // metadata without a transient quota charge.
        if (legacy) requeueLegacy(current);
      }
    }
    return { cleaned,pending:rows.length-cleaned };
  }

  async function cleanupAccount(userId) {
    prepare(`UPDATE cloud_drive_videos SET status='deleted',deleted_at=?,
      cleanup_pending=1,cleanup_after=?,
      cleanup_hold_until=CASE WHEN status='completing' OR
        (status='uploading' AND upload_id IS NULL) THEN ? ELSE NULL END,
      cleanup_revision=cleanup_revision+1
      WHERE owner_user_id=? AND status<>'deleted'`)
      .run(now(),now(),now()+COMPLETION_HOLD_MS,userId);
    await sweep();
    return { pending:prepare(`SELECT COUNT(*) AS count FROM cloud_drive_videos
      WHERE owner_user_id=?`).get(userId).count };
  }

  function cleanupRoom(body,principal) {
    if (mode !== 'session') return result(404,{ error:'Not found' });
    const binding = roomKey(body);
    if (!binding) return result(400,{ error:'Invalid Session binding' });
    const current = now();
    const changed = prepare(`UPDATE cloud_drive_videos SET status='deleted',
      deleted_at=?,cleanup_pending=1,cleanup_after=?,
      cleanup_hold_until=CASE WHEN status='completing' OR
        (status='uploading' AND upload_id IS NULL) THEN ? ELSE NULL END,
      cleanup_revision=cleanup_revision+1
      WHERE owner_user_id=? AND room_key=? AND status<>'deleted'`)
      .run(current,current,current+COMPLETION_HOLD_MS,
        principal.user_id,binding).changes;
    return result(202,{ room_key:binding,deleted:changed });
  }

  async function handle(method,path,body,principal,query) {
    if (!principal) return result(401,{ error:'Authentication required' });
    if (allowedAccountIds && !allowedAccountIds.has(principal.account_id))
      return result(404,{ error:'Not found' });
    if (method === 'POST' && path === `${root}/uploads`)
      return begin(body,principal);
    if (method === 'POST' && path === `${root}/cleanup-room`)
      return cleanupRoom(body,principal);
    if (method === 'GET' && path === root)
      return list(principal,query?.get('before'));
    const part = path.match(new RegExp(`^${root}/uploads/([^/]+)/parts/([^/]+)/sign$`));
    if (part && method === 'POST') return signOne(part[1],part[2],body,principal);
    const upload = path.match(new RegExp(`^${root}/uploads/([^/]+)(/parts|/complete)?$`));
    if (upload && upload[2] === '/parts' && method === 'GET')
      return uploadedParts(upload[1],principal);
    if (upload && upload[2] === '/complete' && method === 'POST')
      return complete(upload[1],principal);
    if (upload && !upload[2] && method === 'DELETE')
      return remove(upload[1],principal);
    const item = path.match(new RegExp(`^${root}/([^/]+)(/play-link|/thumbnail-link)?$`));
    if (item && item[2] === '/play-link' && method === 'GET')
      return play(item[1],principal);
    if (item && item[2] === '/thumbnail-link' && method === 'GET')
      return thumbnail(item[1],principal);
    if (item && !item[2] && method === 'DELETE')
      return remove(item[1],principal);
    return result(404,{ error:'Not found' });
  }

  return { handle,sweep,cleanupAccount };
}

module.exports = { createCloudDriveService };
