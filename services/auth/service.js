'use strict';

const { createHash, createHmac, randomBytes, randomInt, randomUUID, timingSafeEqual } =
  require('node:crypto');
const { createUserSig } = require('./usersig');
const { hashPassword, validPassword, verifyPassword } = require('./password');

const MINUTE = 60_000;
const DAY = 24 * 60 * MINUTE;
const ACCESS_LIFETIME = 15 * MINUTE;
const REFRESH_LIFETIME = 30 * DAY;

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

function createAuthService(options) {
  const { db, sms, secret, imSecret, sdkAppId } = options;
  if (!db || !sms || typeof sms.send !== 'function' ||
      typeof secret !== 'string' || secret.length < 32 ||
      typeof imSecret !== 'string' || imSecret.length < 32 ||
      !Number.isSafeInteger(sdkAppId) || sdkAppId <= 0)
    throw new Error('Auth service configuration is incomplete');
  const now = options.now || Date.now;
  const allowedPhones = new Set(options.allowedPhones || []);
  const sendable = phone => allowedPhones.size === 0 || allowedPhones.has(phone);
  const keyedHash = text => createHmac('sha256', secret).update(text).digest('hex');
  const phoneHash = phone => keyedHash(`phone:${phone}`);
  const ipHash = ip => keyedHash(`ip:${ip || 'unknown'}`);
  const codeHash = (id, phone, code) => keyedHash(`code:${id}:${phone}:${code}`);
  const randomToken = () => randomBytes(32).toString('base64url');
  const dummyHash = hashPassword('unregistered-account-dummy-password');
  let nextCleanupAt = 0;

  function cleanupExpiredRecords() {
    const current = now();
    if (current < nextCleanupAt) return;
    // 验证码、失败尝试与过期会话只留必要的安全追溯窗口，不长期积压。
    db.transaction(() => {
      db.prepare('DELETE FROM sms_challenges WHERE expires_at<?')
        .run(current - 29 * DAY);
      db.prepare('DELETE FROM password_failures WHERE created_at<?')
        .run(current - 29 * DAY);
      db.prepare('DELETE FROM sessions WHERE (revoked_at IS NOT NULL AND revoked_at<?) OR refresh_expires_at<?')
        .run(current - 29 * DAY, current - 29 * DAY);
    })();
    nextCleanupAt = current + DAY;
  }

  function issueSession(user, authenticatedAt = now()) {
    const accessToken = randomToken();
    const refreshToken = randomToken();
    const createdAt = now();
    const id = randomUUID();
    db.prepare(`INSERT INTO sessions(id,user_id,access_hash,access_expires_at,refresh_hash,
      refresh_expires_at,auth_time,created_at) VALUES(?,?,?,?,?,?,?,?)`).run(
      id, user.id, digest(accessToken), createdAt + ACCESS_LIFETIME,
      digest(refreshToken), createdAt + REFRESH_LIFETIME, authenticatedAt, createdAt);
    return {
      access_token: accessToken,
      refresh_token: refreshToken,
      expires_in: ACCESS_LIFETIME / 1000,
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
    return row && !row.revoked_at && !row.disabled_at && row.access_expires_at > now() ? row : null;
  }

  async function sendChallenge(body, ip) {
    const phone = normalizePhone(body?.phone);
    const purpose = body?.purpose || 'login';
    if (!phone || !['login', 'reset'].includes(purpose))
      return json(400, { error: 'Invalid phone or purpose' });
    // 当前没有监护人认证链路；验证码发出前也要求用户确认已满 14 周岁。
    if (body?.age_confirmed_14_plus !== true)
      return json(403, { error: 'Minimum age confirmation required' });
    const hashedPhone = phoneHash(phone);
    const hashedIp = ipHash(ip);
    const current = now();
    // 先限制手机号、出口和重发时间；白名单外给相同的外观响应但不扣短信费用。
    const newest = db.prepare(`SELECT created_at FROM sms_challenges WHERE phone_hash=?
      ORDER BY created_at DESC LIMIT 1`).get(hashedPhone);
    const phoneDaily = db.prepare(`SELECT COUNT(*) AS count FROM sms_challenges
      WHERE phone_hash=? AND created_at>?`).get(hashedPhone, current - DAY).count;
    const ipDaily = db.prepare(`SELECT COUNT(*) AS count FROM sms_challenges
      WHERE ip_hash=? AND created_at>?`).get(hashedIp, current - DAY).count;
    if ((newest && current - newest.created_at < MINUTE) || phoneDaily >= 10 || ipDaily >= 50)
      return json(429, { error: 'Please wait before requesting another code' });
    if (!sendable(phone))
      return json(200, { challenge_id: randomUUID(), expires_in: 300, resend_after: 60 });
    const id = randomUUID();
    const code = String(randomInt(0, 1_000_000)).padStart(6, '0');
    db.prepare(`INSERT INTO sms_challenges(id,phone_hash,ip_hash,purpose,code_hash,
      status,created_at,expires_at) VALUES(?,?,?,?,?,'pending',?,?)`).run(
      id, hashedPhone, hashedIp, purpose, codeHash(id, phone, code), current, current + 5 * MINUTE);
    try {
      await sms.send(phone.slice(3), code);
      db.transaction(() => {
        db.prepare(`UPDATE sms_challenges SET status='superseded'
          WHERE phone_hash=? AND purpose=? AND status='sent'`).run(hashedPhone, purpose);
        db.prepare("UPDATE sms_challenges SET status='sent' WHERE id=? AND status='pending'").run(id);
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

  function verifySms(body) {
    if (body?.age_confirmed_14_plus !== true)
      return json(403, { error: 'Minimum age confirmation required' });
    return db.transaction(() => {
      const phone = consumeChallenge(body, 'login');
      if (!phone) return json(400, { error: 'Invalid or expired verification code' });
      const user = findOrCreateUser(phone);
      if (user.disabled_at) return json(403, { error: 'Account unavailable' });
      return json(200, issueSession(user));
    })();
  }

  async function loginWithPassword(body, ip) {
    const phone = normalizePhone(body?.phone);
    if (!phone || typeof body?.password !== 'string')
      return json(400, { error: 'Invalid login request' });
    if (body?.age_confirmed_14_plus !== true)
      return json(403, { error: 'Minimum age confirmation required' });
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
    db.prepare('DELETE FROM password_failures WHERE phone_hash=?').run(phoneHash(phone));
    return json(200, issueSession(user));
  }

  async function setPassword(body, session) {
    if (!session) return json(401, { error: 'Authentication required' });
    if (now() - session.auth_time > 10 * MINUTE)
      return json(403, { error: 'Sign in again before changing the password' });
    if (!validPassword(body?.new_password))
      return json(400, { error: 'Password must be 15-128 characters' });
    const encoded = await hashPassword(body.new_password);
    db.prepare('UPDATE users SET password_hash=?,updated_at=? WHERE id=?')
      .run(encoded, now(), session.user_id);
    db.prepare('UPDATE sessions SET revoked_at=? WHERE user_id=? AND id!=? AND revoked_at IS NULL')
      .run(now(), session.user_id, session.id);
    return json(200, { password_set: true });
  }

  async function resetPassword(body) {
    if (body?.age_confirmed_14_plus !== true)
      return json(403, { error: 'Minimum age confirmation required' });
    if (!validPassword(body?.new_password))
      return json(400, { error: 'Password must be 15-128 characters' });
    // 先核验一次性码，再做耗 CPU 的密码哈希；否则未登录者可用假码耗尽服务器。
    const phone = db.transaction(() => consumeChallenge(body, 'reset'))();
    if (!phone) return json(400, { error: 'Invalid or expired verification code' });
    const user = db.prepare('SELECT * FROM users WHERE phone_e164=?').get(phone);
    if (!user || user.disabled_at) return json(400, { error: 'Account unavailable' });
    const encoded = await hashPassword(body.new_password);
    return db.transaction(() => {
      db.prepare('UPDATE users SET password_hash=?,updated_at=? WHERE id=?')
        .run(encoded, now(), user.id);
      db.prepare('UPDATE sessions SET revoked_at=? WHERE user_id=? AND revoked_at IS NULL')
        .run(now(), user.id);
      return json(200, issueSession(user));
    })();
  }

  function refresh(body) {
    const token = body?.refresh_token;
    if (typeof token !== 'string' || !/^[A-Za-z0-9_-]{43}$/.test(token))
      return json(401, { error: 'Invalid session' });
    const hash = digest(token);
    return db.transaction(() => {
      const previous = db.prepare('SELECT * FROM sessions WHERE previous_refresh_hash=?').get(hash);
      if (previous && !previous.revoked_at) {
        db.prepare('UPDATE sessions SET revoked_at=? WHERE id=?').run(now(), previous.id);
        return json(401, { error: 'Invalid session' });
      }
      const row = db.prepare(`SELECT sessions.*,users.im_user_id,users.disabled_at
        FROM sessions JOIN users ON users.id=sessions.user_id WHERE refresh_hash=?`).get(hash);
      if (!row || row.revoked_at || row.disabled_at || row.refresh_expires_at <= now())
        return json(401, { error: 'Invalid session' });
      const accessToken = randomToken();
      const refreshToken = randomToken();
      db.prepare(`UPDATE sessions SET access_hash=?,access_expires_at=?,
        previous_refresh_hash=refresh_hash,refresh_hash=? WHERE id=?`).run(
        digest(accessToken), now() + ACCESS_LIFETIME, digest(refreshToken), row.id);
      return json(200, { access_token: accessToken, refresh_token: refreshToken,
        expires_in: ACCESS_LIFETIME / 1000,
        user: { id: row.user_id, im_user_id: row.im_user_id } });
    })();
  }

  async function handle({ method, path, body, headers, ip }) {
    cleanupExpiredRecords();
    const request = body && typeof body === 'object' && !Array.isArray(body) ? body : {};
    if (method === 'POST' && path === '/v1/auth/sms/challenge')
      return sendChallenge(request, ip);
    if (method === 'POST' && path === '/v1/auth/sms/verify') return verifySms(request);
    if (method === 'POST' && path === '/v1/auth/password/login')
      return loginWithPassword(request, ip);
    if (method === 'POST' && path === '/v1/auth/password/reset')
      return resetPassword(request);
    if (method === 'POST' && path === '/v1/auth/refresh') return refresh(request);
    const session = authenticate(headers);
    if (method === 'POST' && path === '/v1/auth/password/set')
      return setPassword(request, session);
    if (method === 'POST' && path === '/v1/auth/logout') {
      if (!session) return json(401, { error: 'Authentication required' });
      db.prepare('UPDATE sessions SET revoked_at=? WHERE id=?').run(now(), session.id);
      return json(200, { logged_out: true });
    }
    if (method === 'GET' && path === '/v1/auth/me') {
      if (!session) return json(401, { error: 'Authentication required' });
      return json(200, { user: { id: session.user_id, im_user_id: session.im_user_id,
        phone_masked: `${session.phone_e164.slice(0, 5)}****${session.phone_e164.slice(-4)}` } });
    }
    if (method === 'GET' && path === '/v1/im/credential') {
      if (!session) return json(401, { error: 'Authentication required' });
      const issuedAt = Math.floor(now() / 1000);
      return json(200, { sdk_app_id: sdkAppId, user_id: session.im_user_id,
        user_sig: createUserSig(sdkAppId, session.im_user_id, imSecret, issuedAt),
        expires_at: issuedAt + 7 * 24 * 60 * 60 });
    }
    return json(404, { error: 'Not found' });
  }

  return { handle, normalizePhone, authenticate, cleanupExpiredRecords };
}

module.exports = { createAuthService, normalizePhone };
