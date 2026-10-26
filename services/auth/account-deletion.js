// Finish a verified deletion request after the external cleanup checklist is complete.
'use strict';

const { createHash, createHmac } = require('node:crypto');

const REQUIRED_EVIDENCE = [
  'tencent_im_deleted',
  'session_sync_deleted',
  'oss_and_ark_assets_reviewed',
  'shared_content_reviewed'
];

function completeAccountDeletion(db, requestId, evidence, secret, completedAt = Date.now()) {
  if (!db || typeof secret !== 'string' || secret.length < 32 ||
      typeof requestId !== 'string' || !/^[0-9a-f-]{36}$/.test(requestId) ||
      !evidence || REQUIRED_EVIDENCE.some(key => evidence[key] !== true ||
        typeof evidence.records?.[key] !== 'string' ||
        evidence.records[key].trim().length < 8 ||
        evidence.records[key].length > 512) ||
      typeof evidence.operator !== 'string' ||
      !/^[A-Za-z0-9._-]{3,64}$/.test(evidence.operator))
    throw new Error('Account deletion completion evidence is incomplete');
  const keyedHash = text => createHmac('sha256', secret).update(text).digest('hex');
  const evidenceHash = createHash('sha256').update(JSON.stringify(evidence)).digest('hex');
  return db.transaction(() => {
    const request = db.prepare(`SELECT * FROM account_deletion_requests
      WHERE id=? AND status='pending'`).get(requestId);
    if (!request?.user_id) throw new Error('Pending deletion request not found');
    const user = db.prepare('SELECT * FROM users WHERE id=?').get(request.user_id);
    if (!user?.disabled_at) throw new Error('Account is not disabled');
    const userId = user.id;
    const cloudPending = db.prepare(`SELECT COUNT(*) AS count FROM cloud_drive_videos
      WHERE owner_user_id=?`)
      .get(userId).count;
    const circlePending = db.prepare(`SELECT COUNT(*) AS count FROM video_circle_posts
      WHERE author_user_id=? AND (status<>'withdrawn' OR cleanup_pending<>0)`)
      .get(userId).count;
    if (cloudPending || circlePending)
      throw new Error('Account media cleanup is not complete');
    const identities = db.prepare('SELECT subject_hash FROM social_identities WHERE user_id=?')
      .all(userId);
    for (const identity of identities)
      db.prepare('DELETE FROM social_bind_intents WHERE subject_hash=?')
        .run(identity.subject_hash);
    if (user.phone_e164) {
      db.prepare('DELETE FROM sms_challenges WHERE phone_hash=?')
        .run(keyedHash(`phone:${user.phone_e164}`));
      db.prepare('DELETE FROM legacy_im_bindings WHERE phone_e164=?')
        .run(user.phone_e164);
    }
    for (const identity of [user.phone_e164, user.account_id, user.id]) {
      if (!identity) continue;
      db.prepare('DELETE FROM password_failures WHERE phone_hash=?')
        .run(keyedHash(`password-login:${identity}`));
    }
    db.prepare('DELETE FROM password_failures WHERE phone_hash=?')
      .run(keyedHash(`account-delete:${userId}`));
    db.prepare(`DELETE FROM desktop_login_requests WHERE approved_user_id=?
      OR approved_session_id IN (SELECT id FROM sessions WHERE user_id=?)
      OR claimed_session_id IN (SELECT id FROM sessions WHERE user_id=?)`)
      .run(userId, userId, userId);
    db.prepare('DELETE FROM friend_mirror WHERE owner_user_id=? OR friend_user_id=?')
      .run(userId, userId);
    db.prepare('DELETE FROM friend_mirror_sync WHERE owner_user_id=?').run(userId);
    db.prepare('DELETE FROM friend_requests WHERE from_user_id=? OR to_user_id=?')
      .run(userId, userId);
    db.prepare('DELETE FROM friendships WHERE left_user_id=? OR right_user_id=?')
      .run(userId, userId);
    db.prepare('DELETE FROM friend_im_sync WHERE left_user_id=? OR right_user_id=?')
      .run(userId, userId);
    db.prepare('DELETE FROM safety_reports WHERE reporter_user_id=? OR target_user_id=?')
      .run(userId, userId);
    db.prepare(`DELETE FROM video_circle_reports WHERE reporter_user_id=? OR
      post_id IN (SELECT id FROM video_circle_posts WHERE author_user_id=?)`)
      .run(userId, userId);
    db.prepare('DELETE FROM video_circle_blocks WHERE viewer_user_id=? OR author_user_id=?')
      .run(userId, userId);
    db.prepare('DELETE FROM video_circle_posts WHERE author_user_id=?').run(userId);
    db.prepare('DELETE FROM cloud_drive_videos WHERE owner_user_id=?').run(userId);
    db.prepare('DELETE FROM profile_im_sync WHERE user_id=?').run(userId);
    db.prepare('DELETE FROM social_identities WHERE user_id=?').run(userId);
    db.prepare('DELETE FROM sessions WHERE user_id=?').run(userId);
    db.prepare('DELETE FROM admin_audit WHERE target_user_id=? OR target_user_id=?')
      .run(userId, user.account_id);
    db.prepare(`UPDATE account_deletion_requests SET user_id=NULL,account_id=NULL,
      im_user_id=NULL,phone_e164=NULL,status='completed',completed_at=?,completion_note=?
      WHERE id=?`).run(completedAt,
        `operator:${evidence.operator};evidence_sha256:${evidenceHash}`, requestId);
    db.prepare('DELETE FROM users WHERE id=?').run(userId);
    return { request_id: requestId, status: 'completed', completed_at: completedAt };
  })();
}

module.exports = { completeAccountDeletion, REQUIRED_EVIDENCE };
