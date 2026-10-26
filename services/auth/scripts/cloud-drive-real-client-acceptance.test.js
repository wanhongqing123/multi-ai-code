'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const { parseArgs,ossHostname,preflight,cleanup } =
  require('./cloud-drive-real-client-acceptance');

const BUCKET = 'maichat-private-cloud-drive-gz-20261025-f346b709';
const ACCOUNT = 'cloudtest_123456abcdef';

test('real acceptance requires exact dedicated bucket and all local inputs', () => {
  assert.equal(parseArgs(['--help']),null);
  assert.throws(() => parseArgs([
    '--env-file=/root/keys','--client-binary=/tmp/client',
    '--system-ca=/etc/ssl/cert.pem','--execute=maichat-ai-video-resource'
  ]),/Explicit dedicated bucket/);
  assert.deepEqual(parseArgs([
    '--env-file=/root/keys','--client-binary=/tmp/client',
    '--system-ca=/etc/ssl/cert.pem',`--execute=${BUCKET}`
  ]),{ 'env-file':'/root/keys','client-binary':'/tmp/client',
    'system-ca':'/etc/ssl/cert.pem',execute:BUCKET });
});

test('internal OSS endpoint is explicit and preserves the default public endpoint', async () => {
  const args = ['--env-file=/root/keys','--client-binary=/tmp/client',
    '--system-ca=/etc/ssl/cert.pem',`--execute=${BUCKET}`];
  assert.equal(parseArgs(args).ossInternal,undefined);
  assert.equal(parseArgs([...args,'--oss-internal']).ossInternal,true);
  assert.throws(() => parseArgs([...args,'--oss-internal','--oss-internal']),
    /Invalid acceptance arguments/);
  assert.equal(ossHostname(),`${BUCKET}.oss-cn-guangzhou.aliyuncs.com`);
  assert.equal(ossHostname(true),
    `${BUCKET}.oss-cn-guangzhou-internal.aliyuncs.com`);

  // The same installed SDK used by the temporary signer must produce an HTTPS
  // V4 part URL on the chosen endpoint, without a network call or real keys.
  const OSS = require('../../oss-media-signer/node_modules/ali-oss');
  for (const internal of [false,true]) {
    const storage = new OSS({ accessKeyId:'dummy-id',
      accessKeySecret:'dummy-secret',bucket:BUCKET,region:'oss-cn-guangzhou',
      authorizationV4:true,secure:true,internal });
    const signed = await storage.signatureUrlV4('PUT',300,{
      headers:{ 'Content-Length':'8388608',
        'Content-MD5':'AAAAAAAAAAAAAAAAAAAAAA==' },
      queries:{ partNumber:1,uploadId:'dummy' }
    },'cloud-drive/cloudtest_123456abcdef/2026-10-25/' +
      '123e4567-e89b-42d3-a456-426614174000.mp4',['content-length']);
    const url = new URL(signed);
    assert.equal(url.protocol,'https:');
    assert.equal(url.hostname,ossHostname(internal));
    assert.equal(url.searchParams.get('x-oss-signature-version'),
      'OSS4-HMAC-SHA256');
  }
});

test('preflight stops on public ACL, version history, and missing abort lifecycle', async () => {
  const makeStorage = ({ acl='private',versionStatus,days=8 } = {}) => ({
    getBucketACL:async () => ({ acl }),
    getBucketVersioning:async () => ({ versionStatus }),
    getBucketLifecycle:async () => ({ rules:[{ status:'Enabled',
      prefix:'cloud-drive/',abortMultipartUpload:{ days } }] })
  });
  await preflight(makeStorage());
  await assert.rejects(preflight(makeStorage({acl:'public-read'})),/unsafe/);
  await assert.rejects(preflight(makeStorage({versionStatus:'Suspended'})),/unsafe/);
  await assert.rejects(preflight(makeStorage({days:1})),/8-day/);
  const shortRootRule = makeStorage();
  shortRootRule.getBucketLifecycle = async () => ({ rules:[
    { status:'Enabled',prefix:'cloud-drive/',abortMultipartUpload:{ days:8 } },
    { status:'Enabled',prefix:'',abortMultipartUpload:{ days:1 } }
  ] });
  await assert.rejects(preflight(shortRootRule),/8-day/);
});

test('cleanup is scoped to one random acceptance account and verifies zero residuals', async () => {
  const prefix = `cloud-drive/${ACCOUNT}/`;
  const key = `${prefix}2026-10-25/123e4567-e89b-42d3-a456-426614174000.mp4`;
  let uploads = [{ name:key,uploadId:'test-upload' }];
  let objects = [{ name:key }];
  const touched = [];
  const storage = {
    list:async options => { assert.equal(options.prefix,prefix); return { objects }; },
    listUploads:async options => { assert.equal(options.prefix,prefix); return { uploads }; },
    abortMultipartUpload:async (name,id) => {
      touched.push(['abort',name,id]); uploads = [];
    },
    delete:async name => { touched.push(['delete',name]); objects = []; }
  };
  await cleanup(storage,ACCOUNT);
  assert.deepEqual(touched,[['abort',key,'test-upload'],['delete',key]]);
  objects = [{ name:'cloud-drive/someone_else/secret.mp4' }];
  await assert.rejects(cleanup(storage,ACCOUNT),/Unexpected data/);
  await assert.rejects(cleanup(storage,'real_user'),/Invalid disposable/);
});
