// 文件作用：处理 MaiChat 账号登录、会话、设备名额与受会话保护的凭据领取权限。
// 供应商密钥由独立凭据进程保管，本进程不转发模型请求。
'use strict';

const { createHash, createHmac, randomBytes, randomInt, randomUUID, timingSafeEqual } =
  require('node:crypto');
const { generatePngQrCode } = require('@juit/qrcode');
const { createUserSig } = require('./usersig');
const { hashPassword, validPassword, verifyPassword } = require('./password');

const MINUTE = 60_000;
const DAY = 24 * 60 * MINUTE;
const ACCESS_LIFETIME = 15 * MINUTE;
const REFRESH_LIFETIME = 30 * DAY;
const DESKTOP_REQUEST_LIFETIME = 2 * MINUTE;
const SOCIAL_BIND_LIFETIME = 10 * MINUTE;
const MOBILE_SLOTS = new Set(['ios_phone', 'android_phone', 'tablet']);

function normalizePhone(input) {
  if (typeof input !== 'string') return '';
  const clean = input.replace(/[ -]/g, '');
  const domestic = clean.startsWith('+86') ? clean.slice(3) : clean;
  return /^1[3-9][0-9]{9}$/.test(domestic) ? `+86${domestic}` : '';
}

function json(statusCode, value) {
  return { statusCode, body: value };
}

function digest(value) {
  return createHash('sha256').update(value).digest('hex');
}

function validBase64Url32(value) {
  return typeof value === 'string' && /^[A-Za-z0-9_-]{43}$/.test(value) &&
    Buffer.from(value, 'base64url').length === 32 &&
    Buffer.from(value, 'base64url').toString('base64url') === value;
}

function mobileDevice(body, requireDeviceSlot = false) {
  if (body?.device_slot === undefined) {
    return !requireDeviceSlot && body?.device_platform === undefined ?
      { device_slot: null, device_platform: null } : null;
  }
  if (!MOBILE_SLOTS.has(body.device_slot)) return null;
  const platform = body.device_slot === 'ios_phone' ? 'ios' :
    body.device_slot === 'android_phone' ? 'android' : body.device_platform;
  if (!['ios', 'android'].includes(platform) ||
      (body.device_platform !== undefined && body.device_platform !== platform))
    return null;
  return { device_slot: body.device_slot, device_platform: platform };
}

function createAuthService(options) {
  const { db, sms, secret, imSecret, sdkAppId, issueArkCredential,
    issueBailianCredential, issueAccountModelKeys, issueAsrRealtimeUrl } = options;
  if (!db || !sms || typeof sms.send !== 'function' ||
      typeof secret !== 'string' || secret.length < 32 ||
      typeof imSecret !== 'string' || imSecret.length < 32 ||
      !Number.isSafeInteger(sdkAppId) || sdkAppId <= 0)
    throw new Error('Auth service configuration is incomplete');
  const now = options.now || Date.now;
  if (options.requireDeviceSlot !== undefined &&
      typeof options.requireDeviceSlot !== 'boolean')
    throw new Error('Invalid device slot policy');
  const requireDeviceSlot = options.requireDeviceSlot === true;
  const socialVerifiers = options.socialVerifiers || {};
  if (typeof socialVerifiers !== 'object' || Array.isArray(socialVerifiers) ||
      ['wechat', 'qq'].some(provider => socialVerifiers[provider] !== undefined &&
        typeof socialVerifiers[provider] !== 'function'))
    throw new Error('Invalid social verifier configuration');
  const arkCredentials = new Map();
  const arkIssuanceInFlight = new Map();
  const bailianCredentials = new Map();
  const bailianIssuanceInFlight = new Map();
  const asrIssuanceTimes = new Map();
  const keyedHash = text => createHmac('sha256', secret).update(text).digest('hex');
  const phoneHash = phone => keyedHash(`phone:${phone}`);
  const ipHash = ip => keyedHash(`ip:${ip || 'unknown'}`);
  const codeHash = (id, phone, code) => keyedHash(`code:${id}:${phone}:${code}`);
  const socialSubjectHash = (provider, appId, subject) =>
    keyedHash(`social:${provider}:${JSON.stringify([appId, subject])}`);
  const randomToken = () => randomBytes(32).toString('base64url');
  const dummyHash = hashPassword('unregistered-account-dummy-password');
  let nextCleanupAt = 0;

  function deviceSlotRequiredError(statusCode) {
    return json(statusCode, { error: 'Sign in with an updated app',
      code: 'DEVICE_SLOT_REQUIRED' });
  }

  function mobileDeviceError(body) {
    return requireDeviceSlot && body?.device_slot === undefined ?
      deviceSlotRequiredError(400) : json(400, { error: 'Invalid device slot' });
  }

  function rejectLegacySession(row) {
    if (!requireDeviceSlot || !row || row.device_slot !== null) return false;
    if (!row.revoked_at) {
      db.prepare(`UPDATE sessions SET revoked_at=?,revoked_reason='device_slot_required'
        WHERE id=? AND revoked_at IS NULL`).run(now(), row.id);
    }
    return true;
  }

  if (requireDeviceSlot) {
    db.prepare(`UPDATE sessions SET revoked_at=?,revoked_reason='device_slot_required'
      WHERE device_slot IS NULL AND revoked_at IS NULL`).run(now());
  }

  function cleanupExpiredRecords() {
    const current = now();
    if (current < nextCleanupAt) return;
    // 验证码、失败尝试与过期会话只留必要的安全追溯窗口，不长期积压。
    db.transaction(() => {
      db.prepare('DELETE FROM sms_challenges WHERE expires_at<?')
        .run(current - 29 * DAY);
      db.prepare('DELETE FROM password_failures WHERE created_at<?')
        .run(current - 29 * DAY);
      db.prepare('DELETE FROM desktop_login_requests WHERE expires_at<?')
        .run(current - DAY);
      db.prepare('DELETE FROM social_bind_intents WHERE expires_at<?')
        .run(current - DAY);
      db.prepare('DELETE FROM social_verification_attempts WHERE created_at<?')
        .run(current - DAY);
      db.prepare('DELETE FROM sessions WHERE (revoked_at IS NOT NULL AND revoked_at<?) OR refresh_expires_at<?')
        .run(current - 29 * DAY, current - 29 * DAY);
    })();
    for (const [userId, times] of asrIssuanceTimes) {
      if (times.at(-1) < current - MINUTE) asrIssuanceTimes.delete(userId);
    }
    nextCleanupAt = current + DAY;
  }

  function issueSession(user, device, authenticatedAt = now(), id = randomUUID()) {
    const accessToken = randomToken();
    const refreshToken = randomToken();
    const createdAt = now();
    if (device.device_slot) {
      db.prepare(`UPDATE sessions SET revoked_at=?,revoked_reason='replaced'
        WHERE user_id=? AND device_slot=? AND revoked_at IS NULL`).run(
        createdAt, user.id, device.device_slot);
    }
    db.prepare(`INSERT INTO sessions(id,user_id,access_hash,access_expires_at,refresh_hash,
      refresh_expires_at,auth_time,created_at,device_slot,device_platform)
      VALUES(?,?,?,?,?,?,?,?,?,?)`).run(
      id, user.id, digest(accessToken), createdAt + ACCESS_LIFETIME,
      digest(refreshToken), createdAt + REFRESH_LIFETIME, authenticatedAt, createdAt,
      device.device_slot, device.device_platform);
    return {
      access_token: accessToken,
      refresh_token: refreshToken,
      expires_in: ACCESS_LIFETIME / 1000,
      device_slot: device.device_slot,
      device_platform: device.device_platform,
      user: { id: user.id, im_user_id: user.im_user_id }
    };
  }

  function authenticate(headers) {
    const raw = headers?.authorization || headers?.Authorization || '';
    if (!/^Bearer [A-Za-z0-9_-]{43}$/.test(raw)) return null;
    const token = raw.slice(7);
    const row = db.prepare(`SELECT sessions.*,users.phone_e164,users.im_user_id,
      users.disabled_at FROM sessions JOIN users ON users.id=sessions.user_id
      WHERE access_hash=?`).get(digest(token));
    if (rejectLegacySession(row)) return null;
    return row && !row.revoked_at && !row.disabled_at && row.access_expires_at > now() ? row : null;
  }

  function authenticationFailure(headers) {
    const raw = headers?.authorization || headers?.Authorization || '';
    if (/^Bearer [A-Za-z0-9_-]{43}$/.test(raw)) {
      const row = db.prepare(`SELECT id,device_slot,revoked_at,revoked_reason
        FROM sessions WHERE access_hash=?`)
        .get(digest(raw.slice(7)));
      if (rejectLegacySession(row)) return deviceSlotRequiredError(401);
      if (row?.revoked_reason === 'replaced')
        return json(401, { error: 'Session replaced by another device',
          code: 'SESSION_REPLACED' });
    }
    return json(401, { error: 'Authentication required' });
  }

  async function createDesktopRequest(body, ip) {
    if (!['mac', 'windows'].includes(body?.platform) ||
        !validBase64Url32(body?.code_challenge))
      return json(400, { error: 'Invalid desktop login request' });
    const current = now();
    const hashedIp = ipHash(ip);
    const count = db.prepare(`SELECT COUNT(*) AS count FROM desktop_login_requests
      WHERE ip_hash=? AND created_at>?`).get(hashedIp, current - 10 * MINUTE).count;
    if (count >= 10)
      return json(429, { error: 'Please wait before requesting another QR code' });
    const id = randomUUID();
    const qrUri = `maichat://auth/desktop?request_id=${id}`;
    const png = Buffer.from(await generatePngQrCode(qrUri, { scale: 5, margin: 4 }));
    if (png.length > 16 * 1024) throw new Error('QR image exceeds response limit');
    return db.transaction(() => {
      const latestCount = db.prepare(`SELECT COUNT(*) AS count FROM desktop_login_requests
        WHERE ip_hash=? AND created_at>?`).get(hashedIp, now() - 10 * MINUTE).count;
      if (latestCount >= 10)
        return json(429, { error: 'Please wait before requesting another QR code' });
      const displayCode = String(randomInt(0, 1_000_000)).padStart(6, '0');
      const createdAt = now();
      db.prepare(`INSERT INTO desktop_login_requests(id,platform,code_challenge,
        display_code,ip_hash,status,created_at,expires_at)
        VALUES(?,?,?,?,?,'pending',?,?)`).run(id, body.platform, body.code_challenge,
        displayCode, hashedIp, createdAt, createdAt + DESKTOP_REQUEST_LIFETIME);
      return json(200, { request_id: id, qr_uri: qrUri,
        qr_png_base64: png.toString('base64'), display_code: displayCode,
        expires_in: DESKTOP_REQUEST_LIFETIME / 1000 });
    })();
  }

  function desktopRequestInfo(id, session) {
    if (!session) return json(401, { error: 'Authentication required' });
    if (!MOBILE_SLOTS.has(session.device_slot))
      return json(403, { error: 'Mobile session required', code: 'MOBILE_SESSION_REQUIRED' });
    if (!/^[0-9a-f-]{36}$/.test(id))
      return json(400, { error: 'Invalid desktop login request' });
    return db.transaction(() => {
      const current = now();
      db.prepare(`UPDATE desktop_login_requests SET status='scanned'
        WHERE id=? AND status='pending' AND expires_at>?`).run(id, current);
      const row = db.prepare(`SELECT platform,display_code,status,expires_at
        FROM desktop_login_requests WHERE id=?`).get(id);
      if (!row) return json(404, { error: 'Desktop login request not found' });
      if (row.expires_at <= current || row.status === 'consumed' ||
          row.status === 'canceled')
        return json(410, { error: 'Desktop login request expired' });
      return json(200, { platform: row.platform, display_code: row.display_code,
        status: row.status, expires_in: Math.ceil((row.expires_at - current) / 1000) });
    })();
  }

  function decideDesktopRequest(body, session, approved) {
    if (!session) return json(401, { error: 'Authentication required' });
    if (!MOBILE_SLOTS.has(session.device_slot))
      return json(403, { error: 'Mobile session required', code: 'MOBILE_SESSION_REQUIRED' });
    if (typeof body?.request_id !== 'string' ||
        !/^[0-9a-f-]{36}$/.test(body.request_id))
      return json(400, { error: 'Invalid desktop login request' });
    return db.transaction(() => {
      const row = db.prepare('SELECT * FROM desktop_login_requests WHERE id=?')
        .get(body.request_id);
      if (!row) return json(404, { error: 'Desktop login request not found' });
      if (row.expires_at <= now() || row.status === 'consumed' ||
          row.status === 'canceled')
        return json(410, { error: 'Desktop login request expired' });
      const status = approved ? 'approved' : 'denied';
      if (row.status !== 'pending' && row.status !== 'scanned') {
        if (row.status === status && row.approved_session_id === session.id)
          return json(200, { status });
        return json(409, { error: 'Desktop login request already decided' });
      }
      db.prepare(`UPDATE desktop_login_requests SET status=?,approved_user_id=?,
        approved_session_id=?,approved_at=? WHERE id=?`).run(
        status, session.user_id, session.id, now(), row.id);
      return json(200, { status });
    })();
  }

  function matchesDesktopVerifier(row, verifier) {
    const actual = createHash('sha256').update(Buffer.from(verifier, 'base64url'))
      .digest('base64url');
    const expected = Buffer.from(row.code_challenge);
    return expected.length === actual.length &&
      timingSafeEqual(Buffer.from(actual), expected);
  }

  function cancelDesktopRequest(body) {
    if (typeof body?.request_id !== 'string' ||
        !/^[0-9a-f-]{36}$/.test(body.request_id) ||
        !validBase64Url32(body?.verifier))
      return json(400, { error: 'Invalid desktop login request' });
    return db.transaction(() => {
      const row = db.prepare('SELECT * FROM desktop_login_requests WHERE id=?')
        .get(body.request_id);
      if (!row) return json(404, { error: 'Desktop login request not found' });
      if (!matchesDesktopVerifier(row, body.verifier))
        return json(401, { error: 'Invalid desktop login verifier' });
      if (row.status === 'canceled') return json(200, { status: 'canceled' });
      if (row.status === 'consumed') {
        if (!row.claimed_session_id)
          return json(409, { error: 'Claimed session cannot be identified' });
        db.prepare(`UPDATE sessions SET revoked_at=?,
          revoked_reason='desktop_login_canceled'
          WHERE id=? AND revoked_at IS NULL`).run(now(), row.claimed_session_id);
        db.prepare(`UPDATE desktop_login_requests SET status='canceled'
          WHERE id=?`).run(row.id);
        return json(200, { status: 'canceled' });
      }
      if (row.expires_at <= now())
        return json(410, { error: 'Desktop login request expired' });
      if (row.status === 'denied')
        return json(409, { error: 'Desktop login request already decided' });
      db.prepare(`UPDATE desktop_login_requests SET status='canceled'
        WHERE id=? AND status IN ('pending','scanned','approved')`).run(row.id);
      return json(200, { status: 'canceled' });
    })();
  }

  function claimDesktopRequest(body) {
    if (typeof body?.request_id !== 'string' ||
        !/^[0-9a-f-]{36}$/.test(body.request_id) ||
        !validBase64Url32(body?.verifier))
      return json(400, { error: 'Invalid desktop login request' });
    return db.transaction(() => {
      const row = db.prepare('SELECT * FROM desktop_login_requests WHERE id=?')
        .get(body.request_id);
      if (!row) return json(404, { error: 'Desktop login request not found' });
      if (!matchesDesktopVerifier(row, body.verifier))
        return json(401, { error: 'Invalid desktop login verifier' });
      if (row.expires_at <= now() || row.status === 'consumed' ||
          row.status === 'canceled')
        return json(410, { error: 'Desktop login request expired' });
      if (row.status === 'denied')
        return json(403, { error: 'Desktop login request denied' });
      if (row.status === 'pending' || row.status === 'scanned')
        return json(202, { status: row.status, retry_after: 2 });
      const approver = db.prepare(`SELECT users.* FROM sessions
        JOIN users ON users.id=sessions.user_id
        WHERE sessions.id=? AND sessions.user_id=?
          AND sessions.revoked_at IS NULL AND sessions.refresh_expires_at>?
          AND sessions.device_slot IN ('ios_phone', 'android_phone', 'tablet')
          AND users.disabled_at IS NULL`).get(
        row.approved_session_id, row.approved_user_id, now());
      if (!approver) return json(403, { error: 'Desktop login approval revoked' });
      const claimedSessionId = randomUUID();
      const session = issueSession(approver,
        { device_slot: row.platform, device_platform: row.platform },
        now(), claimedSessionId);
      db.prepare(`UPDATE desktop_login_requests SET status='consumed',consumed_at=?,
        claimed_session_id=? WHERE id=?`).run(now(), claimedSessionId, row.id);
      return json(200, session);
    })();
  }

  async function sendChallenge(body, ip) {
    const phone = normalizePhone(body?.phone);
    const purpose = body?.purpose || 'login';
    if (!phone || !['login', 'reset', 'social_bind'].includes(purpose))
      return json(400, { error: 'Invalid phone or purpose' });
    // 当前没有监护人认证链路；验证码发出前也要求用户确认已满 14 周岁。
    if (body?.age_confirmed_14_plus !== true)
      return json(403, { error: 'Minimum age confirmation required' });
    const hashedPhone = phoneHash(phone);
    const hashedIp = ipHash(ip);
    const current = now();
    // 所有格式合法的手机号都能收码；先按手机号、来源 IP 和重发间隔限频。
    const newest = db.prepare(`SELECT created_at FROM sms_challenges WHERE phone_hash=?
      ORDER BY created_at DESC LIMIT 1`).get(hashedPhone);
    const phoneDaily = db.prepare(`SELECT COUNT(*) AS count FROM sms_challenges
      WHERE phone_hash=? AND created_at>?`).get(hashedPhone, current - DAY).count;
    const ipDaily = db.prepare(`SELECT COUNT(*) AS count FROM sms_challenges
      WHERE ip_hash=? AND created_at>?`).get(hashedIp, current - DAY).count;
    if ((newest && current - newest.created_at < MINUTE) || phoneDaily >= 10 || ipDaily >= 50)
      return json(429, { error: 'Please wait before requesting another code' });
    const id = randomUUID();
    const code = String(randomInt(0, 1_000_000)).padStart(6, '0');
    db.prepare(`INSERT INTO sms_challenges(id,phone_hash,ip_hash,purpose,code_hash,
      status,created_at,expires_at) VALUES(?,?,?,?,?,'pending',?,?)`).run(
      id, hashedPhone, hashedIp, purpose, codeHash(id, phone, code), current, current + 5 * MINUTE);
    try {
      await sms.send(phone.slice(3), code);
      db.transaction(() => {
        const challenge = db.prepare('SELECT rowid FROM sms_challenges WHERE id=?').get(id);
        const newerSent = db.prepare(`SELECT 1 FROM sms_challenges
          WHERE phone_hash=? AND purpose=? AND status='sent' AND rowid>?
          LIMIT 1`).get(hashedPhone, purpose, challenge.rowid);
        if (newerSent) {
          db.prepare("UPDATE sms_challenges SET status='superseded' WHERE id=? AND status='pending'")
            .run(id);
        } else {
          db.prepare(`UPDATE sms_challenges SET status='superseded'
            WHERE phone_hash=? AND purpose=? AND status='sent' AND rowid<?`)
            .run(hashedPhone, purpose, challenge.rowid);
          db.prepare("UPDATE sms_challenges SET status='sent' WHERE id=? AND status='pending'")
            .run(id);
        }
      })();
    } catch {
      db.prepare("UPDATE sms_challenges SET status='failed' WHERE id=?").run(id);
      return json(503, { error: 'Verification code could not be sent' });
    }
    return json(200, { challenge_id: id, expires_in: 300, resend_after: 60 });
  }

  function consumeChallenge(body, purpose) {
    const phone = normalizePhone(body?.phone);
    const id = body?.challenge_id;
    const code = body?.code;
    if (!phone || typeof id !== 'string' || !/^[0-9a-f-]{36}$/.test(id) ||
        typeof code !== 'string' || !/^[0-9]{6}$/.test(code))
      return null;
    const row = db.prepare('SELECT * FROM sms_challenges WHERE id=?').get(id);
    if (!row || row.status !== 'sent' || row.purpose !== purpose || row.expires_at <= now() ||
        row.attempts >= 5 || row.phone_hash !== phoneHash(phone)) return null;
    const actual = Buffer.from(codeHash(id, phone, code), 'hex');
    const expected = Buffer.from(row.code_hash, 'hex');
    if (actual.length !== expected.length || !timingSafeEqual(actual, expected)) {
      db.prepare('UPDATE sms_challenges SET attempts=attempts+1 WHERE id=?').run(id);
      return null;
    }
    const changed = db.prepare("UPDATE sms_challenges SET status='used' WHERE id=? AND status='sent'")
      .run(id).changes;
    return changed === 1 ? phone : null;
  }

  function findOrCreateUser(phone) {
    let user = db.prepare('SELECT * FROM users WHERE phone_e164=?').get(phone);
    if (user) return user;
    const createdAt = now();
    const id = randomUUID();
    const binding = db.prepare('SELECT im_user_id FROM legacy_im_bindings WHERE phone_e164=?')
      .get(phone);
    const imUserId = binding?.im_user_id || `u${randomBytes(12).toString('hex')}`;
    db.prepare(`INSERT INTO users(id,phone_e164,im_user_id,created_at,updated_at)
      VALUES(?,?,?,?,?)`).run(id, phone, imUserId, createdAt, createdAt);
    if (binding)
      db.prepare('UPDATE legacy_im_bindings SET used_at=? WHERE phone_e164=?')
        .run(createdAt, phone);
    user = db.prepare('SELECT * FROM users WHERE id=?').get(id);
    return user;
  }

  async function socialLogin(provider, body, ip) {
    if (body?.age_confirmed_14_plus !== true)
      return json(403, { error: 'Minimum age confirmation required' });
    const device = mobileDevice(body, true);
    if (!device) return json(400, { error: 'Invalid device slot' });
    const credential = provider === 'wechat' ? body?.code : body?.access_token;
    if (typeof credential !== 'string' || credential.length < 1 ||
        credential.length > 2048 || !/^[\x21-\x7E]+$/.test(credential))
      return json(400, { error: 'Invalid social login request' });
    const verify = socialVerifiers[provider];
    if (typeof verify !== 'function')
      return json(503, { error: 'Social login is unavailable',
        code: 'SOCIAL_PROVIDER_UNAVAILABLE' });
    const hashedIp = ipHash(ip);
    const allowed = db.transaction(() => {
      const current = now();
      const attempts = db.prepare(`SELECT COUNT(*) AS count
        FROM social_verification_attempts WHERE ip_hash=? AND created_at>?`)
        .get(hashedIp, current - 10 * MINUTE).count;
      if (attempts >= 30) return false;
      db.prepare(`INSERT INTO social_verification_attempts(ip_hash,created_at)
        VALUES(?,?)`).run(hashedIp, current);
      return true;
    })();
    if (!allowed) return json(429, { error: 'Please wait before trying again' });
    let verified;
    try {
      verified = await verify(credential);
    } catch {
      return json(503, { error: 'Social login is unavailable',
        code: 'SOCIAL_PROVIDER_UNAVAILABLE' });
    }
    if (verified === null)
      return json(401, { error: 'Social credential is invalid',
        code: 'SOCIAL_CREDENTIAL_INVALID' });
    if (!verified || typeof verified.appId !== 'string' ||
        !/^[A-Za-z0-9_-]{1,128}$/.test(verified.appId) ||
        typeof verified.subject !== 'string' ||
        !/^[\x21-\x7E]{1,256}$/.test(verified.subject))
      return json(503, { error: 'Social login is unavailable',
        code: 'SOCIAL_PROVIDER_UNAVAILABLE' });
    const subjectHash = socialSubjectHash(provider, verified.appId, verified.subject);
    return db.transaction(() => {
      const identity = db.prepare(`SELECT user_id FROM social_identities
        WHERE provider=? AND subject_hash=?`).get(provider, subjectHash);
      if (identity) {
        const user = db.prepare('SELECT * FROM users WHERE id=?').get(identity.user_id);
        if (!user || user.disabled_at)
          return json(403, { error: 'Account unavailable' });
        return json(200, issueSession(user, device));
      }
      const bindingToken = randomToken();
      const current = now();
      db.prepare(`INSERT INTO social_bind_intents(token_hash,provider,subject_hash,
        device_slot,device_platform,created_at,expires_at)
        VALUES(?,?,?,?,?,?,?)`).run(digest(bindingToken), provider, subjectHash,
        device.device_slot, device.device_platform, current,
        current + SOCIAL_BIND_LIFETIME);
      return json(202, { binding_token: bindingToken,
        expires_in: SOCIAL_BIND_LIFETIME / 1000, phone_binding_required: true });
    })();
  }

  function bindSocial(body) {
    if (body?.age_confirmed_14_plus !== true)
      return json(403, { error: 'Minimum age confirmation required' });
    if (!validBase64Url32(body?.binding_token) || !normalizePhone(body?.phone))
      return json(400, { error: 'Invalid social binding request' });
    return db.transaction(() => {
      const intent = db.prepare(`SELECT * FROM social_bind_intents
        WHERE token_hash=?`).get(digest(body.binding_token));
      if (!intent || intent.used_at || intent.expires_at <= now())
        return json(410, { error: 'Social binding expired',
          code: 'SOCIAL_BINDING_EXPIRED' });
      const existingIdentity = db.prepare(`SELECT user_id FROM social_identities
        WHERE provider=? AND subject_hash=?`).get(intent.provider, intent.subject_hash);
      if (existingIdentity)
        return json(409, { error: 'Social identity is already bound',
          code: 'SOCIAL_IDENTITY_ALREADY_BOUND' });
      const phone = consumeChallenge(body, 'social_bind');
      if (!phone)
        return json(400, { error: 'Invalid or expired verification code' });
      const user = findOrCreateUser(phone);
      if (user.disabled_at) return json(403, { error: 'Account unavailable' });
      const providerBinding = db.prepare(`SELECT 1 FROM social_identities
        WHERE provider=? AND user_id=?`).get(intent.provider, user.id);
      if (providerBinding)
        return json(409, { error: 'Account already has this social provider',
          code: 'SOCIAL_PROVIDER_ALREADY_BOUND' });
      db.prepare(`INSERT INTO social_identities(provider,subject_hash,user_id,created_at)
        VALUES(?,?,?,?)`).run(intent.provider, intent.subject_hash, user.id, now());
      db.prepare('UPDATE social_bind_intents SET used_at=? WHERE token_hash=?')
        .run(now(), intent.token_hash);
      return json(200, issueSession(user, {
        device_slot: intent.device_slot, device_platform: intent.device_platform
      }));
    })();
  }

  function verifySms(body) {
    if (body?.age_confirmed_14_plus !== true)
      return json(403, { error: 'Minimum age confirmation required' });
    const device = mobileDevice(body, requireDeviceSlot);
    if (!device) return mobileDeviceError(body);
    return db.transaction(() => {
      const phone = consumeChallenge(body, 'login');
      if (!phone) return json(400, { error: 'Invalid or expired verification code' });
      const user = findOrCreateUser(phone);
      if (user.disabled_at) return json(403, { error: 'Account unavailable' });
      return json(200, issueSession(user, device));
    })();
  }

  async function loginWithPassword(body, ip) {
    const phone = normalizePhone(body?.phone);
    if (!phone || typeof body?.password !== 'string')
      return json(400, { error: 'Invalid login request' });
    if (body?.age_confirmed_14_plus !== true)
      return json(403, { error: 'Minimum age confirmation required' });
    const device = mobileDevice(body, requireDeviceSlot);
    if (!device) return mobileDeviceError(body);
    const failures = db.prepare(`SELECT COUNT(*) AS count FROM password_failures
      WHERE (phone_hash=? OR ip_hash=?) AND created_at>?`).get(
      phoneHash(phone), ipHash(ip), now() - 15 * MINUTE).count;
    if (failures >= 5) return json(429, { error: 'Please wait before trying again' });
    const user = db.prepare('SELECT * FROM users WHERE phone_e164=?').get(phone);
    const encoded = user?.password_hash || await dummyHash;
    const valid = await verifyPassword(body.password, encoded);
    if (!valid || !user || user.disabled_at) {
      db.prepare('INSERT INTO password_failures(phone_hash,ip_hash,created_at) VALUES(?,?,?)')
        .run(phoneHash(phone), ipHash(ip), now());
      return json(401, { error: 'Phone or password is incorrect' });
    }
    return db.transaction(() => {
      const currentUser = db.prepare('SELECT * FROM users WHERE id=?').get(user.id);
      if (!currentUser || currentUser.disabled_at || currentUser.password_hash !== encoded)
        return json(401, { error: 'Phone or password is incorrect' });
      db.prepare('DELETE FROM password_failures WHERE phone_hash=?').run(phoneHash(phone));
      return json(200, issueSession(currentUser, device));
    })();
  }

  async function setPassword(body, session) {
    if (!session) return json(401, { error: 'Authentication required' });
    if (now() - session.auth_time > 10 * MINUTE)
      return json(403, { error: 'Sign in again before changing the password' });
    if (!validPassword(body?.new_password))
      return json(400, { error: 'Password must be 15-128 characters' });
    const encoded = await hashPassword(body.new_password);
    return db.transaction(() => {
      const current = db.prepare(`SELECT sessions.revoked_at,sessions.revoked_reason,
        users.disabled_at FROM sessions JOIN users ON users.id=sessions.user_id
        WHERE sessions.id=?`).get(session.id);
      if (current?.revoked_reason === 'replaced')
        return json(401, { error: 'Session replaced by another device',
          code: 'SESSION_REPLACED' });
      if (!current || current.revoked_at)
        return json(401, { error: 'Authentication required' });
      if (current.disabled_at) return json(403, { error: 'Account unavailable' });
      db.prepare('UPDATE users SET password_hash=?,updated_at=? WHERE id=?')
        .run(encoded, now(), session.user_id);
      db.prepare(`UPDATE sessions SET revoked_at=?,revoked_reason='password_changed'
        WHERE user_id=? AND id!=? AND revoked_at IS NULL`)
        .run(now(), session.user_id, session.id);
      return json(200, { password_set: true });
    })();
  }

  async function resetPassword(body) {
    if (body?.age_confirmed_14_plus !== true)
      return json(403, { error: 'Minimum age confirmation required' });
    if (!validPassword(body?.new_password))
      return json(400, { error: 'Password must be 15-128 characters' });
    const device = mobileDevice(body, requireDeviceSlot);
    if (!device) return mobileDeviceError(body);
    // 先核验一次性码，再做耗 CPU 的密码哈希；否则未登录者可用假码耗尽服务器。
    const phone = db.transaction(() => consumeChallenge(body, 'reset'))();
    if (!phone) return json(400, { error: 'Invalid or expired verification code' });
    const user = db.prepare('SELECT * FROM users WHERE phone_e164=?').get(phone);
    if (!user || user.disabled_at) return json(400, { error: 'Account unavailable' });
    const encoded = await hashPassword(body.new_password);
    return db.transaction(() => {
      const currentUser = db.prepare('SELECT * FROM users WHERE id=?').get(user.id);
      if (!currentUser || currentUser.disabled_at)
        return json(400, { error: 'Account unavailable' });
      db.prepare('UPDATE users SET password_hash=?,updated_at=? WHERE id=?')
        .run(encoded, now(), user.id);
      db.prepare(`UPDATE sessions SET revoked_at=?,revoked_reason='password_reset'
        WHERE user_id=? AND revoked_at IS NULL`).run(now(), user.id);
      return json(200, issueSession(currentUser, device));
    })();
  }

  function refresh(body) {
    const token = body?.refresh_token;
    if (typeof token !== 'string' || !/^[A-Za-z0-9_-]{43}$/.test(token))
      return json(401, { error: 'Invalid session' });
    const hash = digest(token);
    return db.transaction(() => {
      const previous = db.prepare('SELECT * FROM sessions WHERE previous_refresh_hash=?').get(hash);
      if (rejectLegacySession(previous)) return deviceSlotRequiredError(401);
      if (previous?.revoked_reason === 'replaced')
        return json(401, { error: 'Session replaced by another device',
          code: 'SESSION_REPLACED' });
      if (previous && !previous.revoked_at) {
        db.prepare(`UPDATE sessions SET revoked_at=?,revoked_reason='refresh_replay'
          WHERE id=?`).run(now(), previous.id);
        return json(401, { error: 'Invalid session' });
      }
      const row = db.prepare(`SELECT sessions.*,users.im_user_id,users.disabled_at
        FROM sessions JOIN users ON users.id=sessions.user_id WHERE refresh_hash=?`).get(hash);
      if (rejectLegacySession(row)) return deviceSlotRequiredError(401);
      if (row?.revoked_reason === 'replaced')
        return json(401, { error: 'Session replaced by another device',
          code: 'SESSION_REPLACED' });
      if (!row || row.revoked_at || row.disabled_at || row.refresh_expires_at <= now())
        return json(401, { error: 'Invalid session' });
      const accessToken = randomToken();
      const refreshToken = randomToken();
      db.prepare(`UPDATE sessions SET access_hash=?,access_expires_at=?,
        previous_refresh_hash=refresh_hash,refresh_hash=? WHERE id=?`).run(
        digest(accessToken), now() + ACCESS_LIFETIME, digest(refreshToken), row.id);
      return json(200, { access_token: accessToken, refresh_token: refreshToken,
        expires_in: ACCESS_LIFETIME / 1000, device_slot: row.device_slot,
        device_platform: row.device_platform,
        user: { id: row.user_id, im_user_id: row.im_user_id } });
    })();
  }

  async function handle({ method, path, body, headers, ip }) {
    cleanupExpiredRecords();
    const request = body && typeof body === 'object' && !Array.isArray(body) ? body : {};
    if (method === 'POST' && path === '/v1/auth/sms/challenge')
      return sendChallenge(request, ip);
    if (method === 'POST' && path === '/v1/auth/sms/verify') return verifySms(request);
    if (method === 'POST' && path === '/v1/auth/social/wechat/login')
      return socialLogin('wechat', request, ip);
    if (method === 'POST' && path === '/v1/auth/social/qq/login')
      return socialLogin('qq', request, ip);
    if (method === 'POST' && path === '/v1/auth/social/bind')
      return bindSocial(request);
    if (method === 'POST' && path === '/v1/auth/password/login')
      return loginWithPassword(request, ip);
    if (method === 'POST' && path === '/v1/auth/password/reset')
      return resetPassword(request);
    if (method === 'POST' && path === '/v1/auth/refresh') return refresh(request);
    if (method === 'POST' && path === '/v1/auth/desktop/request')
      return createDesktopRequest(request, ip);
    if (method === 'POST' && path === '/v1/auth/desktop/cancel')
      return cancelDesktopRequest(request);
    if (method === 'POST' && path === '/v1/auth/desktop/claim')
      return claimDesktopRequest(request);
    const session = authenticate(headers);
    const protectedRoute =
      (method === 'GET' && path.startsWith('/v1/auth/desktop/request/')) ||
      (method === 'POST' && ['/v1/auth/desktop/approve',
        '/v1/auth/desktop/deny', '/v1/auth/password/set',
        '/v1/auth/logout'].includes(path)) ||
      (method === 'GET' && ['/v1/auth/me', '/v1/im/credential',
        '/v1/media/authorize',
        '/v1/model/seedance-credential', '/v1/model/bailian-credential',
        '/v1/model/api-keys'].includes(path));
    if (protectedRoute && !session) return authenticationFailure(headers);
    if (method === 'GET' && path.startsWith('/v1/auth/desktop/request/'))
      return desktopRequestInfo(path.slice('/v1/auth/desktop/request/'.length), session);
    if (method === 'POST' && path === '/v1/auth/desktop/approve')
      return decideDesktopRequest(request, session, true);
    if (method === 'POST' && path === '/v1/auth/desktop/deny')
      return decideDesktopRequest(request, session, false);
    if (method === 'POST' && path === '/v1/auth/password/set')
      return setPassword(request, session);
    if (method === 'POST' && path === '/v1/auth/logout') {
      if (!session) return json(401, { error: 'Authentication required' });
      db.prepare(`UPDATE sessions SET revoked_at=?,revoked_reason='logout'
        WHERE id=?`).run(now(), session.id);
      return json(200, { logged_out: true });
    }
    if (method === 'GET' && path === '/v1/auth/me') {
      if (!session) return json(401, { error: 'Authentication required' });
      return json(200, { user: { id: session.user_id, im_user_id: session.im_user_id,
        phone_masked: `${session.phone_e164.slice(0, 5)}****${session.phone_e164.slice(-4)}` },
        device_slot: session.device_slot, device_platform: session.device_platform });
    }
    if (method === 'GET' && path === '/v1/im/credential') {
      if (!session) return json(401, { error: 'Authentication required' });
      const issuedAt = Math.floor(now() / 1000);
      return json(200, { sdk_app_id: sdkAppId, user_id: session.im_user_id,
        user_sig: createUserSig(sdkAppId, session.im_user_id, imSecret, issuedAt),
        expires_at: issuedAt + 7 * 24 * 60 * 60 });
    }
    if (method === 'GET' && path === '/v1/media/authorize') {
      if (!session) return json(401, { error: 'Authentication required' });
      return json(200, { allowed: true });
    }
    if (method === 'GET' && path === '/v1/model/api-keys') {
      if (!session) return json(401, { error: 'Authentication required' });
      if (typeof issueAccountModelKeys !== 'function')
        return json(503, { error: 'Main chat model keys are unavailable' });
      try { return json(200, await issueAccountModelKeys()); }
      catch { return json(503, { error: 'Main chat model keys are unavailable' }); }
    }
    if (method === 'POST' && path === '/v1/asr/realtime-url') {
      if (!session) return json(401, { error: 'Authentication required' });
      if (Object.keys(request).length)
        return json(400, { error: 'ASR request parameters are not accepted' });
      if (typeof issueAsrRealtimeUrl !== 'function')
        return json(503, { error: 'Speech recognition is unavailable' });
      const recent = (asrIssuanceTimes.get(session.user_id) || [])
        .filter(time => time > now() - MINUTE);
      if (recent.length >= 10)
        return json(429, { error: 'Speech recognition requests are too frequent' });
      try {
        const issued = issueAsrRealtimeUrl();
        if (!issued || !/^[a-f0-9-]{36}$/.test(issued.voice_id) ||
            typeof issued.url !== 'string' ||
            !issued.url.startsWith('wss://asr.cloud.tencent.com/asr/v2/') ||
            !Number.isSafeInteger(issued.expires_at))
          throw new Error('Invalid ASR URL');
        recent.push(now());
        asrIssuanceTimes.set(session.user_id, recent);
        return json(200, issued);
      } catch { return json(503, { error: 'Speech recognition is unavailable' }); }
    }
    if (method === 'GET' && path === '/v1/model/seedance-credential') {
      if (!session) return json(401, { error: 'Authentication required' });
      if (typeof issueArkCredential !== 'function')
        return json(503, { error: 'Temporary model credentials are unavailable' });
      const cached = arkCredentials.get(session.user_id);
      if (cached && cached.expires_at > Math.floor(now() / 1000) + 90)
        return json(200, cached);
      try {
        let pending = arkIssuanceInFlight.get(session.user_id);
        if (!pending) {
          pending = issueArkCredential();
          arkIssuanceInFlight.set(session.user_id, pending);
        }
        const issued = await pending;
        if (typeof issued?.api_key !== 'string' || issued.api_key.length < 16 ||
            !Number.isSafeInteger(issued.expires_at) ||
            issued.expires_at <= Math.floor(now() / 1000))
          throw new Error('Invalid temporary credential');
        arkCredentials.set(session.user_id, issued);
        return json(200, issued);
      } catch {
        return json(503, { error: 'Temporary model credentials are unavailable' });
      } finally {
        arkIssuanceInFlight.delete(session.user_id);
      }
    }
    if (method === 'GET' && path === '/v1/model/bailian-credential') {
      if (!session) return json(401, { error: 'Authentication required' });
      if (typeof issueBailianCredential !== 'function')
        return json(503, { error: 'Temporary model credentials are unavailable' });
      const cached = bailianCredentials.get(session.user_id);
      if (cached && cached.expires_at > Math.floor(now() / 1000) + 90)
        return json(200, cached);
      try {
        let pending = bailianIssuanceInFlight.get(session.user_id);
        if (!pending) {
          pending = issueBailianCredential();
          bailianIssuanceInFlight.set(session.user_id, pending);
        }
        const issued = await pending;
        if (typeof issued?.api_key !== 'string' ||
            !/^st-[A-Za-z0-9._~+/-]{8,4093}={0,2}$/.test(issued.api_key) ||
            !Number.isSafeInteger(issued.expires_at) ||
            issued.expires_at <= Math.floor(now() / 1000) ||
            !/^(?:ws|llm)-[A-Za-z0-9_-]{1,120}$/.test(issued.wan_workspace_id))
          throw new Error('Invalid temporary credential');
        bailianCredentials.set(session.user_id, issued);
        return json(200, issued);
      } catch {
        return json(503, { error: 'Temporary model credentials are unavailable' });
      } finally {
        bailianIssuanceInFlight.delete(session.user_id);
      }
    }
    return json(404, { error: 'Not found' });
  }

  return { handle, normalizePhone, authenticate, cleanupExpiredRecords };
}

module.exports = { createAuthService, normalizePhone };
