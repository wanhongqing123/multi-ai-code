// MaiChat owns friendship decisions; IM receives the accepted relationship asynchronously.
'use strict';

const { randomUUID } = require('node:crypto');

function json(statusCode, body) { return { statusCode, body }; }
function pair(a, b) { return a < b ? [a, b] : [b, a]; }

function createFriendService({ db, now, normalizePhone, lookupTimes }) {
  function resolve(query) {
    if (typeof query !== 'string' || query.length > 64) return null;
    const phone = normalizePhone(query);
    return phone ? db.prepare(`SELECT id,account_id,nickname,im_user_id FROM users
      WHERE phone_e164=? AND disabled_at IS NULL`).get(phone) :
      db.prepare(`SELECT id,account_id,nickname,im_user_id FROM users
        WHERE account_id=? AND disabled_at IS NULL`).get(query);
  }

  function listFriends(ownerId) {
    const rows = db.prepare(`SELECT users.account_id,users.nickname,users.im_user_id,
      friendships.created_at,friend_im_sync.desired AS pending_im_action
      FROM friendships JOIN users ON users.id=CASE
        WHEN friendships.left_user_id=? THEN friendships.right_user_id
        ELSE friendships.left_user_id END
      LEFT JOIN friend_im_sync ON friend_im_sync.left_user_id=friendships.left_user_id
        AND friend_im_sync.right_user_id=friendships.right_user_id
      WHERE (friendships.left_user_id=? OR friendships.right_user_id=?)
        AND users.disabled_at IS NULL ORDER BY users.id`).all(ownerId, ownerId, ownerId);
    return json(200, { friends: rows });
  }

  function listRequests(ownerId) {
    const incoming = db.prepare(`SELECT friend_requests.id,users.account_id,users.nickname,
      users.im_user_id,friend_requests.created_at FROM friend_requests
      JOIN users ON users.id=friend_requests.from_user_id
      WHERE friend_requests.to_user_id=? AND friend_requests.status='pending'
        AND users.disabled_at IS NULL ORDER BY friend_requests.created_at DESC`).all(ownerId);
    const outgoing = db.prepare(`SELECT friend_requests.id,users.account_id,users.nickname,
      users.im_user_id,friend_requests.created_at FROM friend_requests
      JOIN users ON users.id=friend_requests.to_user_id
      WHERE friend_requests.from_user_id=? AND friend_requests.status='pending'
        AND users.disabled_at IS NULL ORDER BY friend_requests.created_at DESC`).all(ownerId);
    return json(200, { incoming, outgoing });
  }

  function requestFriend(ownerId, query) {
    const peer = resolve(query);
    if (!peer) return json(404, { error: 'MaiChat account not found' });
    if (peer.id === ownerId) return json(400, { error: 'Cannot add your own account' });
    return db.transaction(() => {
      const [left, right] = pair(ownerId, peer.id);
      if (db.prepare(`SELECT 1 FROM friendships WHERE left_user_id=? AND right_user_id=?`)
        .get(left, right)) return json(409, { error: 'Already friends' });
      const opposite = db.prepare(`SELECT id FROM friend_requests WHERE from_user_id=?
        AND to_user_id=? AND status='pending'`).get(peer.id, ownerId);
      if (opposite) return json(409, { error: 'Accept the existing incoming request' });
      const existing = db.prepare(`SELECT id FROM friend_requests WHERE from_user_id=?
        AND to_user_id=? AND status='pending'`).get(ownerId, peer.id);
      if (existing) return json(200, { request_id: existing.id, status: 'pending' });
      const recent = db.prepare(`SELECT COUNT(*) AS count FROM friend_requests
        WHERE from_user_id=? AND created_at>?`).get(ownerId, now() - 60 * 60 * 1000).count;
      if (recent >= 20) return json(429, { error: 'Friend requests are too frequent' });
      const id = randomUUID();
      db.prepare(`INSERT INTO friend_requests(id,from_user_id,to_user_id,status,created_at)
        VALUES(?,?,?,'pending',?)`).run(id, ownerId, peer.id, now());
      return json(201, { request_id: id, status: 'pending' });
    })();
  }

  function decideRequest(ownerId, id, accept) {
    return db.transaction(() => {
      const row = db.prepare(`SELECT * FROM friend_requests WHERE id=? AND to_user_id=?`)
        .get(id, ownerId);
      if (!row) return json(404, { error: 'Friend request not found' });
      if (row.status !== 'pending') return json(409, { error: 'Friend request already decided' });
      const requester = db.prepare('SELECT disabled_at FROM users WHERE id=?')
        .get(row.from_user_id);
      if (requester?.disabled_at) return json(409, { error: 'Account unavailable' });
      db.prepare(`UPDATE friend_requests SET status=?,decided_at=? WHERE id=?`)
        .run(accept ? 'accepted' : 'rejected', now(), id);
      if (accept) {
        const [left, right] = pair(row.from_user_id, ownerId);
        db.prepare(`INSERT OR IGNORE INTO friendships(left_user_id,right_user_id,created_at)
          VALUES(?,?,?)`).run(left, right, now());
        db.prepare(`INSERT INTO friend_im_sync(left_user_id,right_user_id,desired,updated_at)
          VALUES(?,?,'add',?) ON CONFLICT(left_user_id,right_user_id) DO UPDATE SET
          desired='add',updated_at=excluded.updated_at,attempts=0,last_error=NULL,
          revision=friend_im_sync.revision+1`)
          .run(left, right, now());
      }
      return json(200, { status: accept ? 'accepted' : 'rejected' });
    })();
  }

  function deleteFriend(ownerId, friendId) {
    const peer = typeof friendId === 'string' ?
      db.prepare('SELECT id FROM users WHERE account_id=?').get(friendId) : null;
    if (!peer || peer.id === ownerId)
      return json(404, { error: 'Friend not found' });
    return db.transaction(() => {
      const [left, right] = pair(ownerId, peer.id);
      const removed = db.prepare(`DELETE FROM friendships
        WHERE left_user_id=? AND right_user_id=?`).run(left, right).changes;
      if (!removed) return json(404, { error: 'Friend not found' });
      db.prepare(`INSERT INTO friend_im_sync(left_user_id,right_user_id,desired,updated_at)
        VALUES(?,?,'delete',?) ON CONFLICT(left_user_id,right_user_id) DO UPDATE SET
        desired='delete',updated_at=excluded.updated_at,attempts=0,last_error=NULL,
        revision=friend_im_sync.revision+1`)
        .run(left, right, now());
      return json(200, { deleted: true });
    })();
  }

  function handle(method, path, body, session) {
    if (method === 'GET' && path === '/v1/friends') return listFriends(session.user_id);
    if (method === 'GET' && path === '/v1/friends/requests')
      return listRequests(session.user_id);
    if (method === 'POST' && path === '/v1/friends/resolve') {
      const recent = (lookupTimes.get(session.user_id) || [])
        .filter(time => time > now() - 60 * 60 * 1000);
      if (recent.length >= 20)
        return json(429, { error: 'Friend lookup requests are too frequent' });
      recent.push(now());
      lookupTimes.set(session.user_id, recent);
      const peer = resolve(body?.account);
      if (!peer) return json(404, { error: 'MaiChat account not found' });
      if (peer.id === session.user_id)
        return json(400, { error: 'Cannot add your own account' });
      return json(200, { account_id: peer.account_id, nickname: peer.nickname,
        im_user_id: peer.im_user_id });
    }
    if (method === 'POST' && path === '/v1/friends/requests')
      return requestFriend(session.user_id, body?.account);
    if (method === 'POST' && path === '/v1/friends/remove')
      return deleteFriend(session.user_id, body?.account);
    const decision = /^\/v1\/friends\/requests\/([0-9a-f-]{36})\/(accept|reject)$/.exec(path);
    if (method === 'POST' && decision)
      return decideRequest(session.user_id, decision[1], decision[2] === 'accept');
    return null;
  }

  return { handle };
}

module.exports = { createFriendService };
