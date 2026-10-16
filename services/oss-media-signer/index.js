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
const UPLOAD_PURPOSES = Object.freeze({
  ark_asset_input: Object.freeze({ prefix: 'ark-asset-inputs', types: ['image', 'video'] }),
  kling_input: Object.freeze({ prefix: 'kling-inputs', types: ['image', 'video'] }),
  minimax_input: Object.freeze({ prefix: 'minimax-inputs', types: ['image', 'video'] }),
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

function hasBearerToken(headers, expected) {
  const actual = headerValue(headers, 'authorization');
  const expectedHeader = `Bearer ${expected}`;
  const left = Buffer.from(actual);
  const right = Buffer.from(expectedHeader);
  return left.length === right.length && timingSafeEqual(left, right);
}

function requestBody(event) {
  let body = event.body || '';
  if (event.isBase64Encoded) body = Buffer.from(body, 'base64').toString('utf8');
  if (typeof body !== 'string' || Buffer.byteLength(body) > 4096) return null;
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
  const callArk = options.callArk || require('./ark-sign').callArk;
  const fetcher = options.fetcher || fetch;
  let client;

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
    if (method === 'GET' && route === '/portrait-auth-callback')
      return portraitCallbackResponse();
    if (method !== 'POST' || (!route.endsWith('/sign-upload') &&
        !route.endsWith('/ark-assets') && route !== '/credentials' &&
        route !== '/temporary-credentials' && route !== '/account-model-keys'))
      return jsonResponse(404, { error: 'Not found' });

    // 登录服务专用令牌绝不发给 App；其他旧接口仍使用原签名服务令牌。
    const token = route === '/temporary-credentials' || route === '/account-model-keys'
      ? env.MAICHAT_AUTH_SERVICE_TOKEN || ''
      : env.MAICHAT_MEDIA_SERVICE_TOKEN || env.MAICHAT_OSS_SIGNER_TOKEN || '';
    if (token.length < 32) return jsonResponse(503, { error: 'Service token is not configured' });
    if (!hasBearerToken(event.headers, token))
      return jsonResponse(401, { error: 'Unauthorized' });

    const body = requestBody(event);
    if (route === '/account-model-keys') {
      if (body?.action !== 'main_chat')
        return jsonResponse(400, { error: 'Invalid model key request' });
      const glm = env.MAICHAT_GLM_API_KEY || '';
      const deepseek = env.MAICHAT_DEEPSEEK_API_KEY || '';
      if (!glm && !deepseek)
        return jsonResponse(503, { error: 'Main chat models are not configured' });
      return jsonResponse(200, { api_keys: { glm, deepseek } });
    }
    if (route === '/temporary-credentials') {
      if (body?.action === 'bailian_video') {
        const permanentKey = env.MAICHAT_WAN_API_KEY || '';
        const workspace = env.MAICHAT_WAN_WORKSPACE_ID || '';
        if (!permanentKey || !/^(?:ws|llm)-[A-Za-z0-9_-]{1,120}$/.test(workspace))
          return jsonResponse(503, { error: 'Bailian temporary credentials are not configured' });
        // 百炼临时 Key 继承主 Key 的权限，主 Key 始终只留在服务端。
        let response;
        try {
          response = await fetcher(
            'https://dashscope.aliyuncs.com/api/v1/tokens?expire_in_seconds=300', {
              method: 'POST', redirect: 'error',
              headers: { Authorization: `Bearer ${permanentKey}` },
              signal: AbortSignal.timeout(8000)
            });
        } catch {
          return jsonResponse(502, { error: 'Bailian credential issuer is unavailable' });
        }
        if (!response.ok)
          return jsonResponse(502, { error: 'Bailian credential issuer rejected the request' });
        let issued;
        try {
          const raw = await response.text();
          if (Buffer.byteLength(raw) > 2048) throw new Error('Oversized response');
          issued = JSON.parse(raw);
        } catch {
          return jsonResponse(502, { error: 'Bailian credential response is invalid' });
        }
        const nowSeconds = Math.floor(currentTime().getTime() / 1000);
        if (typeof issued.token !== 'string' ||
            !/^st-[A-Za-z0-9._~+/-]{8,4093}={0,2}$/.test(issued.token) ||
            !Number.isSafeInteger(issued.expires_at) ||
            issued.expires_at <= nowSeconds || issued.expires_at > nowSeconds + 360)
          return jsonResponse(502, { error: 'Bailian returned an invalid temporary credential' });
        return jsonResponse(200, { api_key: issued.token, expires_at: issued.expires_at,
          wan_workspace_id: workspace });
      }
      if (body?.action !== 'ark_seedance')
        return jsonResponse(400, { error: 'Invalid temporary credential request' });
      const models = (env.MAICHAT_ARK_MODEL_IDS ||
        env.MAICHAT_SEEDANCE_MODEL_IDS || '').split(',').map(v => v.trim())
        .filter(Boolean);
      if (!models.length || models.length > 10 ||
          !models.every(v => /^doubao-(?:seedance|seedream)-[a-z0-9-]{1,70}$/.test(v)) ||
          new Set(models).size !== models.length ||
          !env.VOLC_ACCESS_KEY_ID || !env.VOLC_SECRET_ACCESS_KEY)
        return jsonResponse(503, { error: 'Ark media temporary credentials are not configured' });
      const issued = await callArk('GetApiKey', {
        DurationSeconds: 300, ResourceType: 'presetendpoint', ResourceIds: models,
        ProjectName: env.VOLC_ARK_PROJECT_NAME || 'default'
      }, { accessKeyId: env.VOLC_ACCESS_KEY_ID,
           secretKey: env.VOLC_SECRET_ACCESS_KEY });
      const result = issued?.Result || {};
      const nowSeconds = Math.floor(currentTime().getTime() / 1000);
      if (typeof result.ApiKey !== 'string' || result.ApiKey.length < 16 ||
          !Number.isSafeInteger(result.ExpiredTime) ||
          result.ExpiredTime <= nowSeconds || result.ExpiredTime > nowSeconds + 360)
        return jsonResponse(502, { error: 'Ark returned an invalid temporary credential' });
      return jsonResponse(200, { api_key: result.ApiKey,
        expires_at: result.ExpiredTime });
    }
    if (route === '/credentials') {
      const keys = {
        // 旧客户端请求 ark 时返回空值，不再下发可长期使用的主 Key。
        ark: '',
        glm: env.MAICHAT_GLM_API_KEY || '',
        glm_video: env.MAICHAT_GLM_API_KEY || '',
        deepseek: env.MAICHAT_DEEPSEEK_API_KEY || '',
        // 百炼万相和可灵只经账号服务领取短期 Key；旧共享令牌不能取回长期 Key。
        wan: '',
        kling: '',
        minimax: env.MAICHAT_MINIMAX_API_KEY || ''
      };
      if (body?.action === 'status') {
        const configured = Object.keys(keys).filter((name) =>
          name !== 'glm_video' && !!keys[name]);
        if (env.MAICHAT_WAN_API_KEY &&
            /^(?:ws|llm)-[A-Za-z0-9_-]{1,120}$/.test(env.MAICHAT_WAN_WORKSPACE_ID || '')) {
          const miniMaxIndex = configured.indexOf('minimax');
          configured.splice(miniMaxIndex < 0 ? configured.length : miniMaxIndex,
            0, 'wan', 'kling');
        }
        return jsonResponse(200, {
          configured,
          assets_configured: !!(env.VOLC_ACCESS_KEY_ID && env.VOLC_SECRET_ACCESS_KEY),
          storage_configured: !!(env.MAICHAT_OSS_BUCKET &&
            /^oss-[a-z0-9-]+$/.test(env.MAICHAT_OSS_REGION || '') &&
            env.ALIBABA_CLOUD_ACCESS_KEY_ID && env.ALIBABA_CLOUD_ACCESS_KEY_SECRET)
        });
      }
      if (body?.action !== 'fetch' || !Array.isArray(body.providers) ||
          body.providers.length < 1 || body.providers.length > Object.keys(keys).length ||
          new Set(body.providers).size !== body.providers.length ||
          !body.providers.every((name) => typeof name === 'string' &&
            Object.hasOwn(keys, name)))
        return jsonResponse(400, { error: 'Choose valid provider names' });
      const selected = {};
      for (const name of body.providers) {
        if (keys[name]) selected[name] = keys[name];
      }
      return jsonResponse(200, { api_keys: selected,
        wan_workspace_id: body.providers.includes('wan')
          ? env.MAICHAT_WAN_WORKSPACE_ID || '' : '' });
    }
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
      const objectKey = `${prefix}/${currentTime().toISOString().slice(0, 10)}/${newID()}${extension}`;
      const contentType = image ? (extension === '.png' ? 'image/png' : 'image/jpeg')
        : video ? (extension === '.mov' ? 'video/quicktime' : 'video/mp4')
          : DOCUMENT_TYPES[extension];
      const uploadHeaders = { 'Content-Type': contentType };
      const uploadUrl = await client.signatureUrlV4(
        'PUT', UPLOAD_TTL_SECONDS, { headers: uploadHeaders }, objectKey);
      const readUrl = await client.signatureUrlV4(
        'GET', READ_TTL_SECONDS, { headers: {} }, objectKey);
      if (!uploadUrl.startsWith('https://') || !readUrl.startsWith('https://'))
        throw new Error('OSS signer returned a non-HTTPS URL');
      return jsonResponse(200, {
        upload_url: uploadUrl,
        upload_headers: uploadHeaders,
        read_url: readUrl,
        object_key: objectKey,
        upload_expires_in: UPLOAD_TTL_SECONDS,
        read_expires_in: READ_TTL_SECONDS
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
