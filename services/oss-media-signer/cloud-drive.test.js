'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const { createHandler } = require('./index');

const SECRET = 'cloud-drive-local-internal-key-at-least-32-characters';
const KEY = 'cloud-drive/account_a/2026-10-24/' +
  '123e4567-e89b-42d3-a456-426614174000.mov';
const ENV = { MAICHAT_CLOUD_DRIVE_ENABLED:'1',
  MAICHAT_CLOUD_DRIVE_INTERNAL_KEY:SECRET,
  MAICHAT_CLOUD_DRIVE_OSS_BUCKET:'private-test-bucket',
  MAICHAT_CLOUD_DRIVE_OSS_REGION:'oss-cn-hangzhou',
  MAICHAT_CLOUD_DRIVE_OSS_ACCESS_KEY_ID:'local-id',
  MAICHAT_CLOUD_DRIVE_OSS_ACCESS_KEY_SECRET:'local-secret' };
const call = (handler,action,body,key = SECRET) => handler({
  rawPath:`/cloud-drive/${action}`,httpMethod:'POST',
  headers:{ 'X-MaiChat-Cloud-Drive-Key':key },
  body:JSON.stringify({ account_id:'account_a',object_key:KEY,...body })
});
const parse = response => JSON.parse(response.body);

test('private multipart signer binds owner, part number, length and MD5', async () => {
  const records = [];
  const handler = createHandler({ env:ENV,makeClient:() => ({
    initMultipartUpload:async (key,options) => {
      records.push(['init',key,options]);
      return { uploadId:'OSSupload_123' };
    },
    signatureUrlV4:async (...args) => {
      records.push(['sign',...args]);
      return `https://private-test-bucket.oss-cn-hangzhou.aliyuncs.com/${KEY}`+
        '?partNumber=1&uploadId=OSSupload_123';
    },
    listParts:async () => ({ parts:[{ PartNumber:'1',Size:String(8*1024*1024),
      ETag:'"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"' }] }),
    completeMultipartUpload:async (...args) => { records.push(['complete',...args]); },
    abortMultipartUpload:async (...args) => { records.push(['abort',...args]); },
    head:async () => ({ res:{ headers:{
      'content-length':'8388608','content-type':'video/quicktime' } } }),
    delete:async key => { records.push(['delete',key]); }
  }) });
  assert.equal((await call(handler,'init',{ content_type:'video/quicktime' },'wrong'))
    .statusCode,403);
  assert.equal((await call(handler,'thumbnail',{ object_key:null })).statusCode,400);
  assert.equal((await call(handler,'init',{ account_id:'account_b',
    content_type:'video/quicktime' })).statusCode,400);
  const initialized = await call(handler,'init',{ content_type:'video/quicktime' });
  assert.equal(parse(initialized).upload_id,'OSSupload_123');
  assert.equal(records[0][2].headers['x-oss-object-acl'],'private');
  const md5 = Buffer.alloc(16,9).toString('base64');
  const signed = await call(handler,'sign-part',{ upload_id:'OSSupload_123',
    part_number:1,expected_bytes:8*1024*1024,content_md5_base64:md5 });
  assert.equal(signed.statusCode,200);
  assert.deepEqual(parse(signed).upload_headers,
    { 'Content-Length':String(8*1024*1024),'Content-MD5':md5 });
  assert.deepEqual(records[1].slice(1,4),['PUT',300,
    { headers:parse(signed).upload_headers,
      queries:{ partNumber:1,uploadId:'OSSupload_123' } }]);
  assert.deepEqual(records[1].at(-1),['content-length']);
  assert.equal((await call(handler,'sign-part',{ upload_id:'OSSupload_123',
    part_number:0,expected_bytes:1,content_md5_base64:md5 })).statusCode,400);
  assert.equal((await call(handler,'sign-part',{ upload_id:'OSSupload_123',
    part_number:1,expected_bytes:8*1024*1024+1,content_md5_base64:md5 }))
    .statusCode,400);
  const listed = await call(handler,'list-parts',{ upload_id:'OSSupload_123' });
  assert.deepEqual(parse(listed).parts,[{ part_number:1,size_bytes:8*1024*1024,
    etag:'"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"' }]);
  const completed = await call(handler,'complete',{ upload_id:'OSSupload_123',
    parts:parse(listed).parts });
  assert.equal(completed.statusCode,200);
  assert.deepEqual(records.find(record => record[0] === 'complete').slice(1),
    [KEY,'OSSupload_123',
      [{ number:1,etag:'"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"' }],
      { headers:{ 'x-oss-forbid-overwrite':'true' } }]);
  assert.equal((await call(handler,'verify',{ expected_bytes:8*1024*1024,
    content_type:'video/quicktime' })).statusCode,200);
  assert.deepEqual(parse(await call(handler,'verify',{ expected_bytes:1,
    content_type:'video/quicktime' })),{ exists:true,
    size_bytes:8*1024*1024,content_type:'video/quicktime' });
  assert.equal(parse(await call(handler,'read',{})).read_expires_in,60);
  assert.equal((await call(handler,'abort',{ upload_id:'OSSupload_123' })).statusCode,200);
  assert.equal((await call(handler,'delete',{})).statusCode,200);
});

test('image uploads and thumbnails use fixed private OSS processing parameters', async () => {
  const seen = [];
  const handler = createHandler({ env:ENV,makeClient:() => ({
    initMultipartUpload:async (key,options) => {
      seen.push(['init',key,options]); return { uploadId:'image_upload_1' };
    },
    signatureUrlV4:async (...args) => {
      seen.push(['thumbnail',...args]);
      return 'https://private-test-bucket.oss-cn-hangzhou.aliyuncs.com/signed';
    },
    head:async () => ({ res:{ headers:{
      'content-length':'20000000','content-type':'image/heic' } } })
  }) });
  const cases = [
    ['jpg','image/jpeg'],['jpeg','image/jpeg'],['png','image/png'],
    ['webp','image/webp'],['heic','image/heic']
  ];
  for (const [extension,type] of cases) {
    const key = KEY.replace(/\.mov$/,`.${extension}`);
    assert.equal((await call(handler,'init',
      { object_key:key,content_type:type })).statusCode,200);
    assert.equal(seen.at(-1)[2].mime,type);
    assert.equal((await call(handler,'init',
      { object_key:key,content_type:'video/mp4' })).statusCode,400);
    assert.equal((await call(handler,'thumbnail',
      { object_key:key,process:'image/resize,w_4000' })).statusCode,200);
    const signed = seen.at(-1);
    assert.deepEqual(signed.slice(1,4),['GET',60,{ headers:{},
      queries:{ 'x-oss-process':'image/resize,m_lfit,w_320,h_320/format,jpg' } }]);
    assert.equal((await call(handler,'thumbnail',
      { account_id:'account_b',object_key:key })).statusCode,400);
  }
  const heic = KEY.replace(/\.mov$/,'.heic');
  assert.equal((await call(handler,'verify',
    { object_key:heic,expected_bytes:20_000_000,content_type:'image/heic' }))
    .statusCode,200);
  assert.equal((await call(handler,'verify',
    { object_key:heic,expected_bytes:20_000_001,content_type:'image/heic' }))
    .statusCode,400);
  assert.equal((await call(handler,'thumbnail',
    { object_key:KEY })).statusCode,200);
  assert.deepEqual(seen.at(-1).slice(1,4),['GET',60,{ headers:{},
    queries:{ 'x-oss-process':'video/snapshot,t_0,f_jpg,w_320' } }]);
});

test('real ali-oss V4 thumbnail URL signs its processing query', async () => {
  const handler = createHandler({ env:ENV });
  const image = KEY.replace(/\.mov$/,'.jpg');
  const reply = await call(handler,'thumbnail',{ object_key:image });
  assert.equal(reply.statusCode,200);
  const url = new URL(parse(reply).thumbnail_url);
  assert.equal(url.searchParams.get('x-oss-process'),
    'image/resize,m_lfit,w_320,h_320/format,jpg');
  assert.equal(url.searchParams.get('x-oss-expires'),'60');
  assert.match(url.searchParams.get('x-oss-signature'),/^[a-f0-9]+$/);
  assert.equal(parse(reply).thumbnail_url.includes('local-secret'),false);
});

test('cloud multipart endpoints are off by default and completion accepts 2 GB part manifest', async () => {
  const off = createHandler({ env:{ ...ENV,MAICHAT_CLOUD_DRIVE_ENABLED:'0' },
    makeClient:() => { throw new Error('must stay off'); } });
  assert.equal((await call(off,'init',{ content_type:'video/quicktime' })).statusCode,404);
  let completed = false;
  const handler = createHandler({ env:ENV,makeClient:() => ({
    completeMultipartUpload:async (_key,_id,parts) => {
      completed = true; assert.equal(parts.length,239);
    }
  }) });
  const parts = Array.from({length:239},(_,index) => ({
    part_number:index+1,size_bytes:8*1024*1024,
    etag:'"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"' }));
  const response = await call(handler,'complete',{ upload_id:'OSSupload_123',parts });
  assert.equal(response.statusCode,200);
  assert.equal(completed,true);
});

test('real ali-oss V4 URL signs the exact part query and Content-Length header', async () => {
  const handler = createHandler({ env:ENV });
  const md5 = Buffer.alloc(16,3).toString('base64');
  const signed = await call(handler,'sign-part',{ upload_id:'OSSupload_123',
    part_number:2,expected_bytes:123456,content_md5_base64:md5 });
  assert.equal(signed.statusCode,200);
  const body = parse(signed);
  const url = new URL(body.upload_url);
  assert.equal(url.hostname,'private-test-bucket.oss-cn-hangzhou.aliyuncs.com');
  assert.equal(url.searchParams.get('partNumber'),'2');
  assert.equal(url.searchParams.get('uploadId'),'OSSupload_123');
  assert.equal(url.searchParams.get('x-oss-additional-headers'),'content-length');
  assert.equal(body.upload_headers['Content-Length'],'123456');
  assert.equal(body.upload_headers['Content-MD5'],md5);
  assert.equal(body.upload_url.includes('local-secret'),false);
});

test('ListParts accepts the single Part object returned by ali-oss XML parsing', async () => {
  const OSS = require('ali-oss');
  const storage = new OSS({
    accessKeyId:'local-id',accessKeySecret:'local-secret',
    bucket:ENV.MAICHAT_CLOUD_DRIVE_OSS_BUCKET,
    region:ENV.MAICHAT_CLOUD_DRIVE_OSS_REGION,
    authorizationV4:true,secure:true
  });
  storage.request = async () => ({ res:{ status:200 },data:{
    Part:{ PartNumber:'1',Size:'8388608',
      ETag:'"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"' },
    IsTruncated:'false'
  } });
  const sdkPage = await storage.listParts(KEY,'OSSupload_123');
  assert.equal(Array.isArray(sdkPage.parts),false);
  const handler = createHandler({ env:ENV,makeClient:() => storage });
  const response = await call(handler,'list-parts',{ upload_id:'OSSupload_123' });
  assert.equal(response.statusCode,200);
  assert.deepEqual(parse(response).parts,[{
    part_number:1,size_bytes:8388608,
    etag:'"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"'
  }]);
});

test('ListUploads parses single and array XML pages and returns only exact-key IDs', async () => {
  const OSS = require('ali-oss');
  const storage = new OSS({
    accessKeyId:'local-id',accessKeySecret:'local-secret',
    bucket:ENV.MAICHAT_CLOUD_DRIVE_OSS_BUCKET,
    region:ENV.MAICHAT_CLOUD_DRIVE_OSS_REGION,
    authorizationV4:true,secure:true
  });
  const queries = [];
  storage.request = async params => {
    queries.push(params.query);
    if (!params.query['upload-id-marker']) return { res:{ status:200 },data:{
      Upload:{ Key:KEY,UploadId:'OSSupload_first' },
      IsTruncated:'true',NextKeyMarker:KEY,
      NextUploadIdMarker:'OSSupload_first'
    } };
    return { res:{ status:200 },data:{
      Upload:[{ Key:KEY,UploadId:'OSSupload_second' },
        { Key:`${KEY}.other`,UploadId:'OSSupload_neighbor' }],
      IsTruncated:'false'
    } };
  };
  const sdkPage = await storage.listUploads({ prefix:KEY,'max-uploads':1000 });
  assert.deepEqual(sdkPage.uploads,[{
    name:KEY,uploadId:'OSSupload_first',initiated:undefined }]);
  assert.equal(sdkPage.isTruncated,true);
  queries.length = 0;
  const handler = createHandler({ env:ENV,makeClient:() => storage });
  const response = await call(handler,'list-uploads',{});
  assert.equal(response.statusCode,200);
  assert.deepEqual(parse(response).upload_ids,
    ['OSSupload_first','OSSupload_second']);
  assert.deepEqual(queries,[
    { prefix:KEY,'max-uploads':1000 },
    { prefix:KEY,'max-uploads':1000,'key-marker':KEY,
      'upload-id-marker':'OSSupload_first' }
  ]);
  assert.equal((await call(handler,'list-uploads',{},'incorrect-key')).statusCode,403);
  assert.equal((await call(handler,'list-uploads',{
    object_key:KEY.replace('account_a','account_b') })).statusCode,400);
});

test('ListUploads fails closed when OSS pagination lacks an advancing marker', async () => {
  const handler = createHandler({ env:ENV,makeClient:() => ({
    listUploads:async () => ({ uploads:[],isTruncated:true,
      nextKeyMarker:KEY,nextUploadIdMarker:'' })
  }) });
  assert.equal((await call(handler,'list-uploads',{})).statusCode,502);
});

test('cloud drive never falls back to media OSS configuration', async () => {
  const legacy = {
    MAICHAT_OSS_BUCKET:'media-bucket',
    MAICHAT_OSS_REGION:'oss-cn-guangzhou',
    ALIBABA_CLOUD_ACCESS_KEY_ID:'media-id',
    ALIBABA_CLOUD_ACCESS_KEY_SECRET:'media-secret'
  };
  const required = [
    'MAICHAT_CLOUD_DRIVE_OSS_BUCKET',
    'MAICHAT_CLOUD_DRIVE_OSS_REGION',
    'MAICHAT_CLOUD_DRIVE_OSS_ACCESS_KEY_ID',
    'MAICHAT_CLOUD_DRIVE_OSS_ACCESS_KEY_SECRET'
  ];
  for (const missing of required) {
    const configuration = { ...ENV,...legacy,[missing]:'' };
    const handler = createHandler({ env:configuration,
      makeClient:() => { throw new Error('Cloud client must not be created'); } });
    const response = await call(handler,'init',{ content_type:'video/quicktime' });
    assert.equal(response.statusCode,503,missing);
  }
  const sharedBucket = createHandler({
    env:{ ...ENV,...legacy,MAICHAT_CLOUD_DRIVE_OSS_BUCKET:legacy.MAICHAT_OSS_BUCKET },
    makeClient:() => { throw new Error('Shared bucket must not be used'); }
  });
  assert.equal((await call(sharedBucket,'init',{
    content_type:'video/quicktime' })).statusCode,503);
});

test('cloud drive and media create distinct clients for their own bucket and credentials', async () => {
  const media = {
    MAICHAT_OSS_BUCKET:'media-bucket',
    MAICHAT_OSS_REGION:'oss-cn-guangzhou',
    ALIBABA_CLOUD_ACCESS_KEY_ID:'media-id',
    ALIBABA_CLOUD_ACCESS_KEY_SECRET:'media-secret'
  };
  const clients = [];
  const handler = createHandler({ env:{ ...ENV,...media },
    authorizeMediaRequest:async () => 'account_a',
    makeClient:configuration => {
      clients.push(configuration);
      if (configuration.bucket === ENV.MAICHAT_CLOUD_DRIVE_OSS_BUCKET)
        return { initMultipartUpload:async () => ({ uploadId:'OSSupload_123' }) };
      return { signatureUrlV4:async () => 'https://media-bucket.oss-cn-guangzhou.aliyuncs.com/signed' };
    }
  });
  assert.equal((await call(handler,'init',{ content_type:'video/quicktime' })).statusCode,200);
  assert.equal((await handler({ rawPath:'/sign-upload',httpMethod:'POST',
    headers:{ Authorization:'Bearer token' },
    body:JSON.stringify({ filename:'clip.mp4',size_bytes:1024,
      purpose:'ark_asset_input' }) })).statusCode,200);
  assert.deepEqual(clients,[
    { bucket:ENV.MAICHAT_CLOUD_DRIVE_OSS_BUCKET,
      region:ENV.MAICHAT_CLOUD_DRIVE_OSS_REGION,
      accessKeyId:ENV.MAICHAT_CLOUD_DRIVE_OSS_ACCESS_KEY_ID,
      accessKeySecret:ENV.MAICHAT_CLOUD_DRIVE_OSS_ACCESS_KEY_SECRET,
      authorizationV4:true,secure:true },
    { bucket:media.MAICHAT_OSS_BUCKET,region:media.MAICHAT_OSS_REGION,
      accessKeyId:media.ALIBABA_CLOUD_ACCESS_KEY_ID,
      accessKeySecret:media.ALIBABA_CLOUD_ACCESS_KEY_SECRET,
      authorizationV4:true,secure:true }
  ]);
});
