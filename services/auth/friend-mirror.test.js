// 文件作用：验证腾讯好友列表完整拉取、失败不覆盖旧镜像和自有账号映射。
'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const { openStore } = require('./store');
const { collectFriendIds, createTencentFriendPageFetcher,
  syncFriendMirror } = require('./friend-mirror');

function addUser(db, id, phone, imUserId) {
  db.prepare(`INSERT INTO users(id,phone_e164,im_user_id,created_at,updated_at)
    VALUES(?,?,?,?,?)`).run(id, phone, imUserId, 1, 1);
}

test('friend list pagination must finish before it can replace a mirror', async () => {
  const pages = [];
  const ids = await collectFriendIds(async (owner, startIndex) => {
    pages.push([owner, startIndex]);
    if (startIndex === 0) return { ActionStatus: 'OK', ErrorCode: 0,
      FriendNum: 2, CompleteFlag: 0, NextStartIndex: 1,
      UserDataItem: [{ To_Account: 'u-b' }] };
    return { ActionStatus: 'OK', ErrorCode: 0,
      FriendNum: 2, CompleteFlag: 1, NextStartIndex: 0,
      UserDataItem: [{ To_Account: 'u-c' }] };
  }, 'u-a');
  assert.deepEqual(ids, ['u-b', 'u-c']);
  assert.deepEqual(pages, [['u-a', 0], ['u-a', 1]]);
  await assert.rejects(collectFriendIds(async () => ({ ActionStatus: 'OK', ErrorCode: 0,
    FriendNum: 3, CompleteFlag: 1, UserDataItem: [{ To_Account: 'u-b' }] }), 'u-a'),
  /changed during pagination/);
});

test('mirror stores MaiChat account IDs and follows full IM friend snapshots', async () => {
  const db = openStore(':memory:');
  try {
    addUser(db, 'a', '+8613900000001', 'u-a');
    addUser(db, 'b', '+8613900000002', 'u-b');
    let aliceFriends = ['u-b', 'external@example.com'];
    const fetchPage = async owner => ({ ActionStatus: 'OK', ErrorCode: 0,
      FriendNum: owner === 'u-a' ? aliceFriends.length : 1,
      CompleteFlag: 1, UserDataItem: (owner === 'u-a' ? aliceFriends : ['u-a'])
        .map(id => ({ To_Account: id })) });
    const first = await syncFriendMirror(db, fetchPage, { now: () => 100 });
    assert.deepEqual(first, { account_count: 2, im_friend_count: 3, mirrored_edges: 2 });
    assert.deepEqual(db.prepare(`SELECT owner_user_id,friend_user_id FROM friend_mirror
      ORDER BY owner_user_id`).all(), [
      { owner_user_id: 'a', friend_user_id: 'b' },
      { owner_user_id: 'b', friend_user_id: 'a' }
    ]);
    aliceFriends = [];
    const second = await syncFriendMirror(db, fetchPage, { now: () => 200 });
    assert.equal(second.mirrored_edges, 1);
    assert.deepEqual(db.prepare(`SELECT owner_user_id,friend_user_id FROM friend_mirror`).all(),
      [{ owner_user_id: 'b', friend_user_id: 'a' }]);
    assert.equal(db.prepare(`SELECT synced_at FROM friend_mirror_sync WHERE owner_user_id='a'`)
      .get().synced_at, 200);
  } finally { db.close(); }
});

test('failed IM response preserves the last completed account snapshot', async () => {
  const db = openStore(':memory:');
  try {
    addUser(db, 'a', '+8613900000001', 'u-a');
    addUser(db, 'b', '+8613900000002', 'u-b');
    await syncFriendMirror(db, async owner => ({ ActionStatus: 'OK', ErrorCode: 0,
      FriendNum: owner === 'u-a' ? 1 : 0, CompleteFlag: 1,
      UserDataItem: owner === 'u-a' ? [{ To_Account: 'u-b' }] : [] }),
    { now: () => 100 });
    await assert.rejects(syncFriendMirror(db, async () => ({ ActionStatus: 'FAIL',
      ErrorCode: 30006 }), { now: () => 200 }), /failed with code 30006/);
    assert.equal(db.prepare('SELECT COUNT(*) AS n FROM friend_mirror').get().n, 1);
    assert.equal(db.prepare(`SELECT synced_at FROM friend_mirror_sync WHERE owner_user_id='a'`)
      .get().synced_at, 100);
  } finally { db.close(); }
});

test('admin fetcher sends a read-only friend_get request', async () => {
  let request;
  const fetchPage = createTencentFriendPageFetcher({ sdkAppId: 123456,
    imSecret: 's'.repeat(32), fetchImpl: async (url, options) => {
      request = { url, options };
      return { ok: true, text: async () => JSON.stringify({ ActionStatus: 'OK',
        ErrorCode: 0, FriendNum: 0, CompleteFlag: 1, UserDataItem: [] }) };
    } });
  await fetchPage('u-a', 0);
  assert.equal(request.url.pathname, '/v4/sns/friend_get');
  assert.equal(request.url.searchParams.get('identifier'), 'administrator');
  assert.deepEqual(JSON.parse(request.options.body), { From_Account: 'u-a', StartIndex: 0 });
  assert.equal(request.options.method, 'POST');
});
