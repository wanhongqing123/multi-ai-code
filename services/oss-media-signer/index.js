// 文件作用：校验登录账号的素材权限，签发 OSS 上传链接并登记 Ark 素材。
// 本进程不下发模型 Key，也不处理模型生成请求或素材文件字节。
'use strict';

const { randomUUID, timingSafeEqual } = require('node:crypto');
const path = require('node:path');

const MAX_VIDEO_BYTES = 200_000_000;
const MAX_IMAGE_BYTES = 30_000_000;
const MAX_DOCUMENT_BYTES = 100_000_000;
const DOCUMENT_TYPES = Object.freeze({
  '.docx': 'application/vnd.openxmlformats-officedocument.wordprocessingml.document',
  '.doc': 'application/msword',
  '.xlsx': 'application/vnd.openxmlformats-officedocument.spreadsheetml.sheet',
  '.xls': 'application/vnd.ms-excel',
  '.pptx': 'application/vnd.openxmlformats-officedocument.presentationml.presentation',
  '.ppt': 'application/vnd.ms-powerpoint',
  '.pdf': 'application/pdf',
  '.txt': 'text/plain',
  '.key': 'application/vnd.apple.keynote',
  '.pages': 'application/vnd.apple.pages',
  '.numbers': 'application/vnd.apple.numbers',
  '.md': 'text/markdown'
});
const UPLOAD_TTL_SECONDS = 15 * 60;
const READ_TTL_SECONDS = 24 * 60 * 60;
const AVATAR_READ_TTL_SECONDS = 5 * 60;
const CIRCLE_READ_TTL_SECONDS = 60;
const CLOUD_READ_TTL_SECONDS = 60;
const CLOUD_PART_TTL_SECONDS = 5 * 60;
const MAX_AVATAR_BYTES = 2_000_000;
const UPLOAD_PURPOSES = Object.freeze({
  ark_asset_input: Object.freeze({ prefix: 'ark-asset-inputs', types: ['image', 'video'] }),
  im_avatar: Object.freeze({ prefix: 'avatars', types: ['image'] }),
  kling_input: Object.freeze({ prefix: 'kling-inputs', types: ['image', 'video'] }),
  minimax_input: Object.freeze({ prefix: 'minimax-inputs', types: ['image', 'video'] }),
  video_circle: Object.freeze({ prefix: 'video-circle', types: ['video'] }),
  seedream_input: Object.freeze({ prefix: 'seedream-inputs', types: ['image'] }),
  wan_input: Object.freeze({ prefix: 'wan-inputs', types: ['image', 'video', 'document'] })
});

function jsonResponse(statusCode, value) {
  return {
    statusCode,
    headers: {
      'Content-Type': 'application/json; charset=utf-8',
      'Cache-Control': 'no-store',
      Pragma: 'no-cache'
    },
    body: JSON.stringify(value)
  };
}

function portraitCallbackResponse() {
  return {
    statusCode: 200,
    headers: {
      'Content-Type': 'text/html; charset=utf-8',
      'Cache-Control': 'no-store',
      'Referrer-Policy': 'no-referrer',
      'Content-Security-Policy': "default-src 'none'; style-src 'unsafe-inline'"
    },
    body: '<!doctype html><html lang="zh"><meta charset="utf-8"><title>认证流程已结束</title>' +
      '<body style="font:16px sans-serif;padding:32px"><h1>认证流程已结束</h1>' +
      '<p>请返回 MaiChat，最终认证结果以应用查询为准。</p></body></html>'
  };
}

function headerValue(headers, name) {
  const entry = Object.entries(headers || {}).find(([key]) => key.toLowerCase() === name);
  return typeof entry?.[1] === 'string' ? entry[1] : '';
}

function requestBody(event, maxBytes = 4096) {
  let body = event.body || '';
  if (event.isBase64Encoded) body = Buffer.from(body, 'base64').toString('utf8');
  if (typeof body !== 'string' || Buffer.byteLength(body) > maxBytes) return null;
  try { return JSON.parse(body); } catch { return null; }
}

function createHandler(options = {}) {
  const env = options.env || process.env;
  const makeClient = options.makeClient || ((configuration) => {
    const OSS = require('ali-oss');
    return new OSS(configuration);
  });
  const newID = options.newID || randomUUID;
  const currentTime = options.currentTime || (() => new Date());
  const callArk = options.callArk || require('../shared/ark-sign').callArk;
  const fetcher = options.fetcher || fetch;
  const authorizeMediaRequest = options.authorizeMediaRequest || (async headers => {
    const bearer = headerValue(headers, 'authorization');
    const port = Number(env.MAICHAT_AUTH_PORT || 9100);
    if (!/^Bearer [A-Za-z0-9_-]{32,256}$/.test(bearer) ||
        !Number.isInteger(port) || port < 1 || port > 65535) return false;
    try {
      const response = await fetcher(`http://127.0.0.1:${port}/v1/media/authorize`, {
        method: 'GET', redirect: 'error',
        headers: { Authorization: bearer }, signal: AbortSignal.timeout(5000)
      });
      if (response.status !== 200) return false;
      const granted = await response.json();
      return granted?.allowed === true &&
        /^[a-z][a-z0-9_-]{2,31}$/.test(granted.account_id || '')
        ? granted.account_id : false;
    } catch { return false; }
  });
  let client;
  let cloudClient;

  function storageClient() {
    if (!client) client = makeClient({
      accessKeyId: env.ALIBABA_CLOUD_ACCESS_KEY_ID,
      accessKeySecret: env.ALIBABA_CLOUD_ACCESS_KEY_SECRET,
      bucket: env.MAICHAT_OSS_BUCKET,
      region: env.MAICHAT_OSS_REGION,
      authorizationV4: true,
      secure: true
    });
    return client;
  }

  function cloudStorageClient() {
    if (!cloudClient) cloudClient = makeClient({
      accessKeyId: env.MAICHAT_CLOUD_DRIVE_OSS_ACCESS_KEY_ID,
      accessKeySecret: env.MAICHAT_CLOUD_DRIVE_OSS_ACCESS_KEY_SECRET,
      bucket: env.MAICHAT_CLOUD_DRIVE_OSS_BUCKET,
      region: env.MAICHAT_CLOUD_DRIVE_OSS_REGION,
      authorizationV4: true,
      secure: true
    });
    return cloudClient;
  }

  function validCircleKey(key, accountId = '') {
    return typeof key === 'string' &&
      /^video-circle\/[a-z][a-z0-9_-]{2,31}\/\d{4}-\d{2}-\d{2}\/[a-f0-9-]{36}\.mp4$/.test(key) &&
      (!accountId || key.startsWith(`video-circle/${accountId}/`));
  }

  function internalCircleRequest(headers) {
    const expected = env.MAICHAT_VIDEO_CIRCLE_INTERNAL_KEY || '';
    const supplied = headerValue(headers, 'x-maichat-video-circle-key');
    return expected.length >= 32 && supplied.length === expected.length &&
      timingSafeEqual(Buffer.from(supplied), Buffer.from(expected));
  }

  function validCloudKey(key, accountId = '') {
    return typeof key === 'string' &&
      /^cloud-drive\/[a-z][a-z0-9_-]{2,31}\/\d{4}-\d{2}-\d{2}\/[a-f0-9-]{36}\.(mp4|mov|jpg|jpeg|png|webp|heic)$/.test(key) &&
      (!accountId || key.startsWith(`cloud-drive/${accountId}/`));
  }

  function cloudContentType(key) {
    if (typeof key !== 'string') return null;
    if (key.endsWith('.mov')) return 'video/quicktime';
    if (key.endsWith('.mp4')) return 'video/mp4';
    if (key.endsWith('.jpg') || key.endsWith('.jpeg')) return 'image/jpeg';
    if (key.endsWith('.png')) return 'image/png';
    if (key.endsWith('.webp')) return 'image/webp';
    if (key.endsWith('.heic')) return 'image/heic';
    return null;
  }

  function internalCloudRequest(headers) {
    const expected = env.MAICHAT_CLOUD_DRIVE_INTERNAL_KEY || '';
    const supplied = headerValue(headers, 'x-maichat-cloud-drive-key');
    return expected.length >= 32 && supplied.length === expected.length &&
      timingSafeEqual(Buffer.from(supplied), Buffer.from(expected));
  }

  function validCloudUploadId(value) {
    return typeof value === 'string' && /^[A-Za-z0-9_+\/=-]{1,512}$/.test(value);
  }

  function cloudPartNumber(value) {
    return Number.isInteger(value) && value >= 1 && value <= 10000;
  }

  return async function handle(eventInput) {
    let event;
    try {
      event = typeof eventInput === 'string' || Buffer.isBuffer(eventInput)
        ? JSON.parse(eventInput.toString()) : eventInput;
    } catch {
      return jsonResponse(400, { error: 'Invalid request' });
    }
    const method = event?.requestContext?.http?.method || event?.httpMethod || event?.method;
    const route = event?.rawPath || event?.path || '/sign-upload';
    const circleAction = ['/video-circle/verify', '/video-circle/read',
      '/video-circle/delete'].find(suffix => route.endsWith(suffix));
    if (circleAction) {
      if (method !== 'POST' || env.MAICHAT_VIDEO_CIRCLE_ENABLED !== '1')
        return jsonResponse(404, { error: 'Not found' });
      if (!internalCircleRequest(event.headers))
        return jsonResponse(403, { error: 'Internal authorization required' });
      const body = requestBody(event);
      const key = body?.object_key;
      if (!validCircleKey(key)) return jsonResponse(400, { error: 'Invalid circle object' });
      if (!env.MAICHAT_OSS_BUCKET || !/^oss-[a-z0-9-]+$/.test(env.MAICHAT_OSS_REGION || '') ||
          !env.ALIBABA_CLOUD_ACCESS_KEY_ID || !env.ALIBABA_CLOUD_ACCESS_KEY_SECRET)
        return jsonResponse(503, { error: 'Storage signer is not configured' });
      try {
        const storage = storageClient();
        if (circleAction === '/video-circle/verify') {
          const accountId = body?.account_id;
          const expectedBytes = body?.expected_bytes;
          if (typeof accountId !== 'string' || !validCircleKey(key, accountId) ||
              !Number.isSafeInteger(expectedBytes) || expectedBytes < 1 ||
              expectedBytes > MAX_VIDEO_BYTES)
            return jsonResponse(400, { error: 'Invalid circle upload verification' });
          const head = await storage.head(key);
          const headers = head?.res?.headers || head?.headers || {};
          const bytes = Number(headers['content-length']);
          const type = headers['content-type'];
          return jsonResponse(200, { exists: bytes === expectedBytes && type === 'video/mp4',
            size_bytes: bytes, content_type: type });
        }
        if (circleAction === '/video-circle/read') {
          const url = await storage.signatureUrlV4(
            'GET', CIRCLE_READ_TTL_SECONDS, { headers: {} }, key);
          if (!url.startsWith('https://')) throw new Error('Invalid signed read URL');
          return jsonResponse(200, { read_url: url,
            read_expires_in: CIRCLE_READ_TTL_SECONDS });
        }
        await storage.delete(key);
        return jsonResponse(200, { deleted: true });
      } catch {
        return jsonResponse(502, { error: 'Circle storage operation failed' });
      }
    }
    const cloudAction = ['/cloud-drive/init', '/cloud-drive/sign-part',
      '/cloud-drive/list-parts', '/cloud-drive/list-uploads',
      '/cloud-drive/complete', '/cloud-drive/abort',
      '/cloud-drive/verify', '/cloud-drive/read', '/cloud-drive/thumbnail',
      '/cloud-drive/delete']
      .find(suffix => route === suffix);
    if (cloudAction) {
      if (method !== 'POST' || env.MAICHAT_CLOUD_DRIVE_ENABLED !== '1')
        return jsonResponse(404, { error: 'Not found' });
      if (!internalCloudRequest(event.headers))
        return jsonResponse(403, { error: 'Internal authorization required' });
      const body = requestBody(event,32768);
      const key = body?.object_key;
      const accountId = body?.account_id;
      const type = cloudContentType(key);
      if (!/^[a-z][a-z0-9_-]{2,31}$/.test(accountId || '') ||
          !validCloudKey(key,accountId) ||
          ((cloudAction === '/cloud-drive/init' ||
            cloudAction === '/cloud-drive/verify') && body.content_type !== type) ||
          (cloudAction === '/cloud-drive/verify' &&
            (!Number.isSafeInteger(body.expected_bytes) || body.expected_bytes < 1 ||
              body.expected_bytes > (type?.startsWith('image/') ? 20_000_000 : 2_000_000_000))) ||
          (['/cloud-drive/sign-part','/cloud-drive/list-parts',
            '/cloud-drive/complete','/cloud-drive/abort'].includes(cloudAction) &&
            !validCloudUploadId(body.upload_id)))
        return jsonResponse(400, { error: 'Invalid private video object' });
      const bucket = env.MAICHAT_CLOUD_DRIVE_OSS_BUCKET || '';
      const region = env.MAICHAT_CLOUD_DRIVE_OSS_REGION || '';
      if (!/^[a-z0-9][a-z0-9-]{1,61}[a-z0-9]$/.test(bucket) ||
          bucket === env.MAICHAT_OSS_BUCKET ||
          !/^oss-[a-z0-9-]+$/.test(region) ||
          !env.MAICHAT_CLOUD_DRIVE_OSS_ACCESS_KEY_ID ||
          !env.MAICHAT_CLOUD_DRIVE_OSS_ACCESS_KEY_SECRET)
        return jsonResponse(503, { error: 'Cloud storage signer is not configured' });
      try {
        const storage = cloudStorageClient();
        if (cloudAction === '/cloud-drive/init') {
          const response = await storage.initMultipartUpload(key, {
            mime:type, headers: { 'x-oss-object-acl':'private',
              'x-oss-forbid-overwrite':'true' } });
          if (!validCloudUploadId(response?.uploadId))
            throw new Error('OSS returned an invalid upload ID');
          return jsonResponse(200,{ upload_id:response.uploadId });
        }
        if (cloudAction === '/cloud-drive/sign-part') {
          const number = body.part_number;
          const bytes = body.expected_bytes;
          const md5 = body.content_md5_base64;
          if (!cloudPartNumber(number) || !Number.isSafeInteger(bytes) ||
              bytes < 1 || bytes > 8*1024*1024 ||
              typeof md5 !== 'string' || !/^[A-Za-z0-9+/]{22}==$/.test(md5) ||
              Buffer.from(md5,'base64').length !== 16)
            return jsonResponse(400,{ error:'Invalid upload part' });
          const headers = { 'Content-Length':String(bytes), 'Content-MD5':md5 };
          const url = await storage.signatureUrlV4('PUT',CLOUD_PART_TTL_SECONDS,
            { headers, queries:{ partNumber:number, uploadId:body.upload_id } },
            key,['content-length']);
          if (typeof url !== 'string' || !url.startsWith('https://'))
            throw new Error('OSS returned an invalid part URL');
          return jsonResponse(200,{ upload_url:url,upload_headers:headers });
        }
        if (cloudAction === '/cloud-drive/list-parts') {
          const parts = [];
          let marker = 0;
          do {
            const page = await storage.listParts(key,body.upload_id,
              { 'max-parts':1000,'part-number-marker':marker });
            // ali-oss returns the parsed XML Part node directly. A single
            // Part is an object; multiple Parts are an array.
            const pageParts = Array.isArray(page.parts) ? page.parts
              : page.parts ? [page.parts] : [];
            for (const part of pageParts) {
              const number = Number(part.PartNumber ?? part.partNumber);
              const bytes = Number(part.Size ?? part.size);
              const etag = part.ETag ?? part.etag;
              if (!cloudPartNumber(number) || !Number.isSafeInteger(bytes) ||
                  bytes < 1 || typeof etag !== 'string' || !etag)
                throw new Error('OSS returned an invalid part');
              parts.push({ part_number:number,size_bytes:bytes,etag });
            }
            if (page.isTruncated === true || page.isTruncated === 'true') {
              const next = Number(page.nextPartNumberMarker);
              if (!cloudPartNumber(next) || next <= marker)
                throw new Error('OSS part pagination did not advance');
              marker = next;
            } else break;
          } while (parts.length <= 10000);
          if (parts.length > 10000) throw new Error('Too many OSS parts');
          parts.sort((a,b) => a.part_number-b.part_number);
          return jsonResponse(200,{ parts });
        }
        if (cloudAction === '/cloud-drive/list-uploads') {
          // ListUploads is bucket scoped. The internal caller may inspect only
          // its authenticated account's exact, validated object key.
          const uploadIds = [];
          const seenIds = new Set();
          const seenMarkers = new Set();
          let keyMarker;
          let uploadIdMarker;
          for (let pageNumber = 0; pageNumber < 100; pageNumber++) {
            const query = { prefix:key,'max-uploads':1000 };
            if (keyMarker !== undefined) {
              query['key-marker'] = keyMarker;
              query['upload-id-marker'] = uploadIdMarker;
            }
            const page = await storage.listUploads(query);
            if (!Array.isArray(page?.uploads))
              throw new Error('OSS returned an invalid upload list');
            for (const upload of page.uploads) {
              if (typeof upload?.name !== 'string' ||
                  !upload.name.startsWith(key))
                throw new Error('OSS returned an upload outside the requested prefix');
              if (upload.name !== key) continue;
              if (!validCloudUploadId(upload.uploadId) ||
                  seenIds.has(upload.uploadId))
                throw new Error('OSS returned an invalid or duplicate upload ID');
              seenIds.add(upload.uploadId);
              uploadIds.push(upload.uploadId);
              if (uploadIds.length > 10000)
                throw new Error('Too many exact-key OSS uploads');
            }
            if (page.isTruncated !== true && page.isTruncated !== false)
              throw new Error('OSS returned an invalid pagination state');
            if (!page.isTruncated)
              return jsonResponse(200,{ upload_ids:uploadIds });
            const nextKey = page.nextKeyMarker;
            const nextId = page.nextUploadIdMarker;
            if (typeof nextKey !== 'string' || !nextKey.startsWith(key) ||
                typeof nextId !== 'string' || !validCloudUploadId(nextId))
              throw new Error('OSS upload pagination did not advance');
            const marker = `${nextKey}\u0000${nextId}`;
            if (seenMarkers.has(marker))
              throw new Error('OSS upload pagination repeated a marker');
            seenMarkers.add(marker);
            keyMarker = nextKey;
            uploadIdMarker = nextId;
          }
          throw new Error('Too many OSS upload pages');
        }
        if (cloudAction === '/cloud-drive/complete') {
          const parts = body.parts;
          if (!Array.isArray(parts) || parts.length < 1 || parts.length > 10000 ||
              parts.some((part,index) =>
                part?.part_number !== index+1 || typeof part.etag !== 'string' ||
                !/^\"?[A-Fa-f0-9]{32}\"?$/.test(part.etag)))
            return jsonResponse(400,{ error:'Invalid completion parts' });
          await storage.completeMultipartUpload(key,body.upload_id,
            parts.map(part => ({ number:part.part_number,etag:part.etag })),
            { headers:{ 'x-oss-forbid-overwrite':'true' } });
          return jsonResponse(200,{ completed:true });
        }
        if (cloudAction === '/cloud-drive/abort') {
          try { await storage.abortMultipartUpload(key,body.upload_id); }
          catch (error) {
            if (error?.status !== 404 && error?.code !== 'NoSuchUpload') throw error;
          }
          return jsonResponse(200,{ aborted:true });
        }
        if (cloudAction === '/cloud-drive/verify') {
          let head;
          try { head = await storage.head(key); }
          catch (error) {
            if (error?.status === 404 || error?.code === 'NoSuchKey')
              return jsonResponse(200,{ exists:false });
            throw error;
          }
          const headers = head?.res?.headers || head?.headers || {};
          const bytes = Number(headers['content-length']);
          const contentType = headers['content-type'];
          // HEAD success means an object exists even when metadata mismatches.
          // Cleanup must never treat an oversized or mistyped object as absent.
          return jsonResponse(200,{ exists:true,
            size_bytes: bytes, content_type: contentType });
        }
        if (cloudAction === '/cloud-drive/read') {
          const url = await storage.signatureUrlV4('GET',CLOUD_READ_TTL_SECONDS,
            { headers: {} },key);
          if (typeof url !== 'string' || !url.startsWith('https://'))
            throw new Error('Invalid signed read URL');
          return jsonResponse(200,{ read_url:url,
            read_expires_in:CLOUD_READ_TTL_SECONDS });
        }
        if (cloudAction === '/cloud-drive/thumbnail') {
          const transform = type.startsWith('image/')
            ? 'image/resize,m_lfit,w_320,h_320/format,jpg'
            : 'video/snapshot,t_0,f_jpg,w_320,ar_auto';
          // Processing must be in the V4 signature. Appending it to an
          // ordinary read URL would allow signature rejection or bypass.
          const url = await storage.signatureUrlV4('GET',CLOUD_READ_TTL_SECONDS,
            { headers:{},queries:{ 'x-oss-process':transform } },key);
          if (typeof url !== 'string' || !url.startsWith('https://'))
            throw new Error('Invalid signed thumbnail URL');
          return jsonResponse(200,{ thumbnail_url:url,
            thumbnail_expires_in:CLOUD_READ_TTL_SECONDS });
        }
        await storage.delete(key);
        return jsonResponse(200,{ deleted:true });
      } catch {
        return jsonResponse(502,{ error:'Cloud storage operation failed' });
      }
    }
    if (method === 'GET' && route === '/portrait-auth-callback')
      return portraitCallbackResponse();
    if (method === 'GET' && route.startsWith('/avatars/')) {
      if (!/^\/avatars\/(?:[a-z][a-z0-9_-]{2,31}\/)?\d{4}-\d{2}-\d{2}\/[a-f0-9-]{36}\.(jpg|jpeg|png)$/.test(route))
        return jsonResponse(404, { error: 'Not found' });
      if (!env.MAICHAT_OSS_BUCKET || !/^oss-[a-z0-9-]+$/.test(env.MAICHAT_OSS_REGION || '') ||
          !env.ALIBABA_CLOUD_ACCESS_KEY_ID || !env.ALIBABA_CLOUD_ACCESS_KEY_SECRET)
        return jsonResponse(503, { error: 'Storage signer is not configured' });
      try {
        if (!client) client = makeClient({
          accessKeyId: env.ALIBABA_CLOUD_ACCESS_KEY_ID,
          accessKeySecret: env.ALIBABA_CLOUD_ACCESS_KEY_SECRET,
          bucket: env.MAICHAT_OSS_BUCKET,
          region: env.MAICHAT_OSS_REGION,
          authorizationV4: true,
          secure: true
        });
        const readUrl = await client.signatureUrlV4(
          'GET', AVATAR_READ_TTL_SECONDS, { headers: {} }, route.slice(1));
        if (!readUrl.startsWith('https://')) throw new Error('Invalid OSS URL');
        return { statusCode: 302, headers: { Location: readUrl,
          'Cache-Control': 'public, max-age=60', 'Referrer-Policy': 'no-referrer' },
        body: '' };
      } catch { return jsonResponse(502, { error: 'Could not read the avatar' }); }
    }
    const mediaRoute = route.endsWith('/sign-upload') || route.endsWith('/ark-assets');
    if (method !== 'POST' || !mediaRoute)
      return jsonResponse(404, { error: 'Not found' });

    const accountId = await authorizeMediaRequest(event.headers);
    if (!accountId)
      return jsonResponse(401, { error: 'Account media access is required' });

    const body = requestBody(event);
    if (route.endsWith('/ark-assets')) {
      const accessKeyId = env.VOLC_ACCESS_KEY_ID || '';
      const secretKey = env.VOLC_SECRET_ACCESS_KEY || '';
      if (!accessKeyId || !secretKey)
        return jsonResponse(503, { error: 'Ark Assets credentials are not configured' });
      const action = body?.action;
      const project = env.VOLC_ARK_PROJECT_NAME || 'default';
      const groupType = body?.group_type || 'AIGC';
      if (groupType !== 'AIGC' && groupType !== 'LivenessFace')
        return jsonResponse(400, { error: 'Invalid asset group type' });
      let payload;
      if (action === 'create_group' && validText(body.name, 64)) {
        payload = { Name: body.name, Description: validText(body.description, 300)
          ? body.description : '', GroupType: 'AIGC', ProjectName: project };
      } else if (action === 'create_asset' && validId(body.group_id, 'group-') &&
                 validHttpsUrl(body.url) &&
                 (!Object.hasOwn(body, 'asset_type') ||
                  body.asset_type === 'Image' || body.asset_type === 'Video') &&
                 (!Object.hasOwn(body, 'expected_group_type') ||
                  body.expected_group_type === 'AIGC' ||
                  body.expected_group_type === 'LivenessFace') &&
                 validText(body.name || body.asset_type || 'Image', 64)) {
        const assetType = body.asset_type || 'Image';
        payload = { GroupId: body.group_id, URL: body.url, AssetType: assetType,
          Name: body.name || assetType, ProjectName: project };
      } else if (action === 'get_asset' && validId(body.asset_id, 'asset-')) {
        payload = { Id: body.asset_id, ProjectName: project };
      } else if (action === 'get_group' && validId(body.group_id, 'group-')) {
        payload = { Id: body.group_id, ProjectName: project };
      } else if (action === 'list_groups') {
        payload = { Filter: { GroupType: groupType }, MaxResults: 100, ProjectName: project };
        if (body.next_token && validText(body.next_token, 2048))
          payload.NextToken = body.next_token;
      } else if (action === 'list_assets') {
        payload = { Filter: { GroupType: groupType }, MaxResults: 100, ProjectName: project };
        if (body.group_id && validId(body.group_id, 'group-'))
          payload.Filter.GroupIds = [body.group_id];
        if (body.next_token && validText(body.next_token, 2048))
          payload.NextToken = body.next_token;
      } else if (action === 'begin_real_validation') {
        payload = { CallbackURL: env.MAICHAT_PORTRAIT_CALLBACK_URL ||
          'https://ichat.life/maichat/portrait-auth-callback', ProjectName: project };
        if (!validHttpsUrl(payload.CallbackURL))
          return jsonResponse(503, { error: 'Portrait callback URL is not configured' });
      } else if (action === 'get_real_validation' &&
                 validText(body.byted_token, 512)) {
        payload = { BytedToken: body.byted_token, ProjectName: project };
      } else {
        return jsonResponse(400, { error: 'Invalid Ark Assets request' });
      }
      const arkAction = {
        create_group: 'CreateAssetGroup', create_asset: 'CreateAsset',
        get_asset: 'GetAsset', get_group: 'GetAssetGroup',
        list_groups: 'ListAssetGroups', list_assets: 'ListAssets',
        begin_real_validation: 'CreateVisualValidateSession',
        get_real_validation: 'GetVisualValidateResult'
      }[action];
      try {
        if (action === 'create_asset' && Object.hasOwn(body, 'expected_group_type')) {
          const lookup = await callArk('GetAssetGroup',
            { Id: body.group_id, ProjectName: project }, { accessKeyId, secretKey });
          const lookupError = lookup?.ResponseMetadata?.Error;
          if (lookupError)
            return jsonResponse(502, { error: lookupError.Message || 'Ark rejected the request',
              provider_code: lookupError.Code || '',
              request_id: lookup.ResponseMetadata.RequestId || '' });
          const group = lookup?.Result;
          if (!group || typeof group !== 'object' ||
              (group.Id && group.Id !== body.group_id) ||
              (group.GroupType !== 'AIGC' && group.GroupType !== 'LivenessFace'))
            return jsonResponse(502, { error: 'Ark returned an invalid asset group' });
          if (group.GroupType !== body.expected_group_type)
            return jsonResponse(409, { error: 'Asset group type does not match' });
        }
        const response = await callArk(arkAction, payload, { accessKeyId, secretKey });
        const error = response?.ResponseMetadata?.Error;
        if (error) return jsonResponse(502, { error: error.Message || 'Ark rejected the request',
          provider_code: error.Code || '', request_id: response.ResponseMetadata.RequestId || '' });
        const result = response?.Result || response;
        if (!result || typeof result !== 'object')
          return jsonResponse(502, { error: 'Ark returned an invalid response' });
        return jsonResponse(200, { result });
      } catch (error) {
        const metadata = error?.response?.data?.ResponseMetadata || {};
        return jsonResponse(502, { error: 'Ark Assets request failed',
          provider_code: metadata.Error?.Code || '',
          provider_message: metadata.Error?.Message || '',
          request_id: metadata.RequestId || '',
          provider_http_status: error?.response?.status || 0 });
      }
    }
    const bucket = env.MAICHAT_OSS_BUCKET || '';
    const region = env.MAICHAT_OSS_REGION || '';
    if (!bucket || !/^oss-[a-z0-9-]+$/.test(region) ||
        !env.ALIBABA_CLOUD_ACCESS_KEY_ID || !env.ALIBABA_CLOUD_ACCESS_KEY_SECRET)
      return jsonResponse(503, { error: 'Storage signer is not configured' });
    const filename = body?.filename;
    const size = body?.size_bytes;
    const extension = typeof filename === 'string'
      ? path.extname(filename).toLowerCase() : '';
    const video = ['.mp4', '.mov'].includes(extension);
    const image = ['.jpg', '.jpeg', '.png'].includes(extension);
    const document = Object.hasOwn(DOCUMENT_TYPES, extension);
    if ((!video && !image && !document) || !Number.isSafeInteger(size) || size < 1 ||
        size > (image ? MAX_IMAGE_BYTES : video ? MAX_VIDEO_BYTES : MAX_DOCUMENT_BYTES))
      return jsonResponse(400, { error: 'Provide supported media or a document within its size limit' });
    const type = image ? 'image' : video ? 'video' : 'document';
    const purpose = body.purpose;
    if (typeof purpose !== 'string' || !Object.hasOwn(UPLOAD_PURPOSES, purpose) ||
        !UPLOAD_PURPOSES[purpose].types.includes(type))
      return jsonResponse(400, { error: 'Invalid upload purpose for this file type' });
    if (purpose === 'im_avatar' && size > MAX_AVATAR_BYTES)
      return jsonResponse(400, { error: 'Avatar is too large' });
    if (purpose === 'video_circle' && extension !== '.mp4')
      return jsonResponse(400, { error: 'Video circle requires MP4' });
    if (purpose === 'video_circle' &&
        (env.MAICHAT_VIDEO_CIRCLE_ENABLED !== '1' ||
         (env.MAICHAT_VIDEO_CIRCLE_INTERNAL_KEY || '').length < 32 ||
         !/^[a-z][a-z0-9_-]{2,31}$/.test(accountId)))
      return jsonResponse(503, { error: 'Video circle is not available' });
    const prefix = UPLOAD_PURPOSES[purpose].prefix;

    try {
      if (!client) client = makeClient({
        accessKeyId: env.ALIBABA_CLOUD_ACCESS_KEY_ID,
        accessKeySecret: env.ALIBABA_CLOUD_ACCESS_KEY_SECRET,
        bucket,
        region,
        authorizationV4: true,
        secure: true
      });
      const objectId = newID();
      if (purpose === 'im_avatar' && !/^[a-f0-9-]{36}$/.test(objectId))
        throw new Error('Invalid avatar object ID');
      if (purpose === 'video_circle' && !/^[a-f0-9-]{36}$/.test(objectId))
        throw new Error('Invalid circle object ID');
      const ownerPrefix = typeof accountId === 'string' &&
        /^[a-z][a-z0-9_-]{2,31}$/.test(accountId) ? `${accountId}/` : '';
      const objectKey = `${prefix}/${ownerPrefix}${currentTime().toISOString().slice(0, 10)}/${objectId}${extension}`;
      const contentType = image ? (extension === '.png' ? 'image/png' : 'image/jpeg')
        : video ? (extension === '.mov' ? 'video/quicktime' : 'video/mp4')
          : DOCUMENT_TYPES[extension];
      const uploadHeaders = { 'Content-Type': contentType,
        ...(purpose === 'video_circle' ? { 'x-oss-forbid-overwrite': 'true' } : {}) };
      const uploadUrl = await client.signatureUrlV4(
        'PUT', UPLOAD_TTL_SECONDS, { headers: uploadHeaders }, objectKey);
      const readUrl = purpose === 'video_circle' ? null : await client.signatureUrlV4(
        'GET', READ_TTL_SECONDS, { headers: {} }, objectKey);
      if (!uploadUrl.startsWith('https://') ||
          (readUrl !== null && !readUrl.startsWith('https://')))
        throw new Error('OSS signer returned a non-HTTPS URL');
      return jsonResponse(200, {
        upload_url: uploadUrl,
        upload_headers: uploadHeaders,
        ...(readUrl ? { read_url: readUrl } : {}),
        ...(purpose === 'im_avatar' ? {
          avatar_url: `https://ichat.life/maichat/${objectKey}` } : {}),
        object_key: objectKey,
        upload_expires_in: UPLOAD_TTL_SECONDS,
        ...(readUrl ? { read_expires_in: READ_TTL_SECONDS } : {})
      });
    } catch {
      return jsonResponse(502, { error: 'Could not sign the media upload' });
    }
  };
}

function validText(value, maximumBytes) {
  return typeof value === 'string' && value.trim().length > 0 &&
    Buffer.byteLength(value) <= maximumBytes;
}

function validId(value, prefix) {
  return typeof value === 'string' && value.startsWith(prefix) && value.length <= 128 &&
    /^[a-zA-Z0-9_-]+$/.test(value);
}

function validHttpsUrl(value) {
  if (typeof value !== 'string' || value.length > 3000) return false;
  try {
    const url = new URL(value);
    return url.protocol === 'https:' && !!url.hostname && !url.username && !url.password;
  } catch { return false; }
}

exports.handler = createHandler();
exports.createHandler = createHandler;
