// 文件作用：从腾讯 IM 服务端只读拉取好友列表，保存按 MaiChat 账号 ID 索引的迁移镜像。
// 镜像不参与当前 IM 鉴权或好友申请，完整拉取成功后才替换该账号的旧快照。
'use strict';

const { randomInt } = require('node:crypto');
const Database = require('better-sqlite3');
const { openStore } = require('./store');
const { createUserSig } = require('./usersig');

const MAX_PAGES = 1000;
const MAX_RESPONSE_BYTES = 8 * 1024 * 1024;
const DEFAULT_ORIGIN = 'https://console.tim.qq.com';

function validImId(value) {
  return typeof value === 'string' && /^[A-Za-z0-9_-]{1,128}$/.test(value);
}

function validFriendId(value) {
  return typeof value === 'string' && value.length > 0 && value.length <= 128 &&
    !/[\x00-\x1F\x7F]/.test(value);
}

async function collectFriendIds(fetchPage, ownerImId) {
  const friends = new Set();
  let startIndex = 0;
  for (let pageNumber = 0; pageNumber < MAX_PAGES; pageNumber++) {
    const result = await fetchPage(ownerImId, startIndex);
    if (result?.ActionStatus !== 'OK' || result.ErrorCode !== 0)
      throw new Error(`IM friend list failed with code ${result?.ErrorCode ?? 'unknown'}`);
    if (!Number.isSafeInteger(result.FriendNum) || result.FriendNum < 0 ||
        ![0, 1].includes(result.CompleteFlag))
      throw new Error('IM friend list metadata is invalid');
    const entries = result.UserDataItem ?? [];
    if (!Array.isArray(entries)) throw new Error('IM friend list entries are invalid');
    for (const entry of entries) {
      if (!validFriendId(entry?.To_Account))
        throw new Error('IM friend list contains an invalid account ID');
      friends.add(entry.To_Account);
    }
    if (result.CompleteFlag === 1) {
      if (friends.size !== result.FriendNum)
        throw new Error('IM friend list changed during pagination');
      return [...friends];
    }
    if (!Number.isSafeInteger(result.NextStartIndex) || result.NextStartIndex <= startIndex)
      throw new Error('IM friend list pagination did not advance');
    startIndex = result.NextStartIndex;
  }
  throw new Error('IM friend list exceeds the page limit');
}

function createTencentFriendPageFetcher({ sdkAppId, imSecret, adminUser = 'administrator',
  origin = DEFAULT_ORIGIN, fetchImpl = fetch }) {
  if (!Number.isSafeInteger(sdkAppId) || sdkAppId <= 0 ||
      typeof imSecret !== 'string' || imSecret.length < 32 ||
      !validImId(adminUser) || !origin.startsWith('https://'))
    throw new Error('IM friend mirror configuration is invalid');
  const userSig = createUserSig(sdkAppId, adminUser, imSecret, Math.floor(Date.now() / 1000));
  return async (ownerImId, startIndex) => {
    if (!validImId(ownerImId) || !Number.isSafeInteger(startIndex) || startIndex < 0)
      throw new Error('Invalid IM friend list request');
    const url = new URL('/v4/sns/friend_get', origin);
    url.search = new URLSearchParams({ sdkappid: String(sdkAppId), identifier: adminUser,
      usersig: userSig, random: String(randomInt(1, 0xFFFFFFFF)), contenttype: 'json' }).toString();
    const response = await fetchImpl(url, { method: 'POST', redirect: 'error',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ From_Account: ownerImId, StartIndex: startIndex }),
      signal: AbortSignal.timeout(10_000) });
    if (!response.ok) throw new Error(`IM friend list HTTP ${response.status}`);
    const text = await response.text();
    if (text.length > MAX_RESPONSE_BYTES) throw new Error('IM friend list response is too large');
    try { return JSON.parse(text); }
    catch { throw new Error('IM friend list response is invalid JSON'); }
  };
}

function replaceMirrorForAccount(db, ownerUserId, friendUserIds, imFriendCount, syncedAt) {
  const replace = db.transaction(() => {
    db.prepare('DELETE FROM friend_mirror WHERE owner_user_id=?').run(ownerUserId);
    const insert = db.prepare(`INSERT INTO friend_mirror
      (owner_user_id,friend_user_id,source,observed_at) VALUES(?,?,'tencent_im',?)`);
    for (const friendUserId of friendUserIds)
      insert.run(ownerUserId, friendUserId, syncedAt);
    db.prepare(`INSERT INTO friend_mirror_sync
      (owner_user_id,synced_at,im_friend_count,mirrored_count) VALUES(?,?,?,?)
      ON CONFLICT(owner_user_id) DO UPDATE SET synced_at=excluded.synced_at,
        im_friend_count=excluded.im_friend_count,mirrored_count=excluded.mirrored_count`)
      .run(ownerUserId, syncedAt, imFriendCount, friendUserIds.length);
  });
  replace();
}

async function syncFriendMirror(db, fetchPage, { now = Date.now, dryRun = false } = {}) {
  const users = db.prepare('SELECT id,im_user_id FROM users ORDER BY id').all();
  const accountIds = new Map(users.map(user => [user.im_user_id, user.id]));
  const summary = { account_count: 0, im_friend_count: 0, mirrored_edges: 0 };
  for (const owner of users) {
    const imFriendIds = await collectFriendIds(fetchPage, owner.im_user_id);
    const mapped = [...new Set(imFriendIds.map(id => accountIds.get(id)))]
      .filter(id => id && id !== owner.id);
    if (!dryRun)
      replaceMirrorForAccount(db, owner.id, mapped, imFriendIds.length, now());
    summary.account_count++;
    summary.im_friend_count += imFriendIds.length;
    summary.mirrored_edges += mapped.length;
  }
  return summary;
}

async function main() {
  if (!['--run', '--dry-run'].includes(process.argv[2]) || process.argv.length !== 3)
    throw new Error('Usage: node friend-mirror.js --run|--dry-run');
  const dryRun = process.argv[2] === '--dry-run';
  const filename = process.env.MAICHAT_AUTH_DB_PATH;
  if (!filename) throw new Error('Auth database path is not configured');
  const db = dryRun ? new Database(filename, { readonly: true }) : openStore(filename);
  try {
    const fetchPage = createTencentFriendPageFetcher({
      sdkAppId: Number(process.env.MAICHAT_IM_SDK_APP_ID),
      imSecret: process.env.MAICHAT_IM_SECRET_KEY,
      adminUser: process.env.MAICHAT_IM_ADMIN_USER || 'administrator',
      origin: process.env.MAICHAT_IM_REST_ORIGIN || DEFAULT_ORIGIN
    });
    const result = await syncFriendMirror(db, fetchPage, { dryRun });
    process.stdout.write(`${JSON.stringify({ mode: dryRun ? 'dry-run' : 'run', ...result })}\n`);
  } finally { db.close(); }
}

if (require.main === module) main().catch(error => {
  process.stderr.write(`${error.message}\n`);
  process.exitCode = 1;
});

module.exports = { collectFriendIds, createTencentFriendPageFetcher, syncFriendMirror };
