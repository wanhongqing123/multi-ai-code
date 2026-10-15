// 文件作用：为 iOS 与 Android 模拟器运行正式 App 提供两个本机测试账号。
// 短信验证和模型签发均留在内存中；模型付费入口关闭，账号口令仅写入本机受限文件。
'use strict';

const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { randomBytes } = require('node:crypto');
const { openStore } = require('./store');
const { createAuthService } = require('./service');
const { createServer } = require('./server');

const port = 18190;
const phones = ['13900000001', '13900000002'];
const root = path.join(os.tmpdir(), 'maichat-local-ui');
const accountFile = path.join(root, 'accounts.json');
const db = openStore(':memory:');
const codes = new Map();
const service = createAuthService({
  db,
  sms: { async send(phone, code) { codes.set(phone, code); } },
  secret: randomBytes(32).toString('hex'),
  imSecret: randomBytes(32).toString('hex'),
  sdkAppId: 123456789,
  modelAllowedPhones: [],
  issueArkCredential: async () => { throw new Error('Paid models are disabled locally'); },
  issueBailianCredential: async () => { throw new Error('Paid models are disabled locally'); }
});

async function call(method, endpoint, body, token = '') {
  const result = await service.handle({
    method, path: endpoint, body, ip: '127.0.0.1',
    headers: token ? { authorization: `Bearer ${token}` } : {}
  });
  if (result.statusCode !== 200) {
    throw new Error(`Local account setup failed at ${endpoint}: HTTP ${result.statusCode}`);
  }
  return result.body;
}

async function start() {
  const accounts = [];
  for (const phone of phones) {
    const challenge = await call('POST', '/v1/auth/sms/challenge', {
      phone, age_confirmed_14_plus: true
    });
    const code = codes.get(phone);
    if (!code) throw new Error('Local mock SMS did not produce a code');
    const session = await call('POST', '/v1/auth/sms/verify', {
      phone, challenge_id: challenge.challenge_id, code, age_confirmed_14_plus: true
    });
    const password = randomBytes(24).toString('base64url');
    await call('POST', '/v1/auth/password/set', { new_password: password },
      session.access_token);
    accounts.push({ phone, password, im_user_id: session.user.im_user_id });
  }
  fs.mkdirSync(root, { recursive: true, mode: 0o700 });
  fs.writeFileSync(accountFile, JSON.stringify(accounts, null, 2), { mode: 0o600 });
  fs.chmodSync(accountFile, 0o600);
  const server = createServer(service);
  server.listen(port, '127.0.0.1', () => {
    process.stdout.write(`Local UI auth listening on 127.0.0.1:${port}; accounts: ${accountFile}\n`);
  });
}

start().catch(error => {
  process.stderr.write(`${error.message}\n`);
  process.exitCode = 1;
});
