'use strict';

const { randomUUID, timingSafeEqual } = require('node:crypto');
const path = require('node:path');

const MAX_VIDEO_BYTES = 200_000_000;
const UPLOAD_TTL_SECONDS = 15 * 60;
const READ_TTL_SECONDS = 24 * 60 * 60;

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
    if (method !== 'POST' || (!route.endsWith('/sign-upload') &&
        !route.endsWith('/ark-assets') && route !== '/credentials'))
      return jsonResponse(404, { error: 'Not found' });

    const token = env.MAICHAT_MEDIA_SERVICE_TOKEN || env.MAICHAT_OSS_SIGNER_TOKEN || '';
    if (token.length < 32) return jsonResponse(503, { error: 'Service token is not configured' });
    if (!hasBearerToken(event.headers, token))
      return jsonResponse(401, { error: 'Unauthorized' });

    const body = requestBody(event);
    if (route === '/credentials') {
      const keys = {
        ark: env.MAICHAT_ARK_API_KEY || '',
        glm: env.MAICHAT_GLM_API_KEY || '',
        deepseek: env.MAICHAT_DEEPSEEK_API_KEY || '',
        wan: env.MAICHAT_WAN_API_KEY || '',
        kling: env.MAICHAT_KLING_API_KEY || '',
        minimax: env.MAICHAT_MINIMAX_API_KEY || ''
      };
      if (body?.action === 'status')
        return jsonResponse(200, {
          configured: Object.keys(keys).filter((name) => !!keys[name]),
          assets_configured: !!(env.VOLC_ACCESS_KEY_ID && env.VOLC_SECRET_ACCESS_KEY),
          storage_configured: !!(env.MAICHAT_OSS_BUCKET &&
            /^oss-[a-z0-9-]+$/.test(env.MAICHAT_OSS_REGION || '') &&
            env.ALIBABA_CLOUD_ACCESS_KEY_ID && env.ALIBABA_CLOUD_ACCESS_KEY_SECRET)
        });
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
      let payload;
      if (action === 'create_group' && validText(body.name, 64)) {
        payload = { Name: body.name, Description: validText(body.description, 300)
          ? body.description : '', GroupType: 'AIGC', ProjectName: project };
      } else if (action === 'create_asset' && validId(body.group_id, 'group-') &&
                 validHttpsUrl(body.url) && validText(body.name || 'Image', 64)) {
        payload = { GroupId: body.group_id, URL: body.url, AssetType: 'Image',
          Name: body.name || 'Image', ProjectName: project };
      } else if (action === 'get_asset' && validId(body.asset_id, 'asset-')) {
        payload = { Id: body.asset_id, ProjectName: project };
      } else if (action === 'list_groups') {
        payload = { Filter: { GroupType: 'AIGC' }, MaxResults: 100, ProjectName: project };
        if (body.next_token && validText(body.next_token, 2048))
          payload.NextToken = body.next_token;
      } else if (action === 'list_assets') {
        payload = { Filter: { GroupType: 'AIGC' }, MaxResults: 100, ProjectName: project };
        if (body.group_id && validId(body.group_id, 'group-'))
          payload.Filter.GroupIds = [body.group_id];
        if (body.next_token && validText(body.next_token, 2048))
          payload.NextToken = body.next_token;
      } else {
        return jsonResponse(400, { error: 'Invalid Ark Assets request' });
      }
      const arkAction = {
        create_group: 'CreateAssetGroup', create_asset: 'CreateAsset',
        get_asset: 'GetAsset', list_groups: 'ListAssetGroups', list_assets: 'ListAssets'
      }[action];
      try {
        const response = await callArk(arkAction, payload, { accessKeyId, secretKey });
        const error = response?.ResponseMetadata?.Error;
        if (error) return jsonResponse(502, { error: error.Message || 'Ark rejected the request',
          provider_code: error.Code || '', request_id: response.ResponseMetadata.RequestId || '' });
        const result = response?.Result || response;
        if (!result || typeof result !== 'object')
          return jsonResponse(502, { error: 'Ark returned an invalid response' });
        return jsonResponse(200, { result });
      } catch (error) {
        return jsonResponse(502, { error: 'Ark Assets request failed',
          provider_code: error?.response?.data?.ResponseMetadata?.Error?.Code || '',
          provider_message: error?.response?.data?.ResponseMetadata?.Error?.Message || '' });
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
    if (!['.mp4', '.mov'].includes(extension) || !Number.isSafeInteger(size) ||
        size < 1 || size > MAX_VIDEO_BYTES)
      return jsonResponse(400, { error: 'Provide an MP4/MOV file of at most 200 MB' });

    try {
      if (!client) client = makeClient({
        accessKeyId: env.ALIBABA_CLOUD_ACCESS_KEY_ID,
        accessKeySecret: env.ALIBABA_CLOUD_ACCESS_KEY_SECRET,
        bucket,
        region,
        authorizationV4: true,
        secure: true
      });
      const objectKey = `seedance-inputs/${currentTime().toISOString().slice(0, 10)}/${newID()}${extension}`;
      const contentType = extension === '.mov' ? 'video/quicktime' : 'video/mp4';
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
      return jsonResponse(502, { error: 'Could not sign the video upload' });
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
