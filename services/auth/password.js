'use strict';

const { randomBytes, scrypt: callbackScrypt, timingSafeEqual } = require('node:crypto');
const { promisify } = require('node:util');

const scrypt = promisify(callbackScrypt);
const COST = Object.freeze({ N: 1 << 15, r: 8, p: 3, maxmem: 64 * 1024 * 1024 });

function validPassword(password) {
  // 密码登录是单因素；允许长密码短语与粘贴，不强制字符种类。
  return typeof password === 'string' && [...password].length >= 15 &&
    [...password].length <= 128 && Buffer.byteLength(password, 'utf8') <= 512;
}

async function hashPassword(password) {
  if (!validPassword(password)) throw new Error('Password must be 15-128 characters');
  const salt = randomBytes(16);
  const hash = await scrypt(password, salt, 32, COST);
  return `scrypt$${COST.N}$${COST.r}$${COST.p}$${salt.toString('base64url')}$` +
    hash.toString('base64url');
}

async function verifyPassword(password, encoded) {
  if (typeof password !== 'string' || Buffer.byteLength(password, 'utf8') > 512 ||
      typeof encoded !== 'string') return false;
  const parts = encoded.split('$');
  if (parts.length !== 6 || parts[0] !== 'scrypt' ||
      parts[1] !== String(COST.N) || parts[2] !== String(COST.r) ||
      parts[3] !== String(COST.p)) return false;
  let salt, expected;
  try {
    salt = Buffer.from(parts[4], 'base64url');
    expected = Buffer.from(parts[5], 'base64url');
  } catch { return false; }
  if (salt.length !== 16 || expected.length !== 32) return false;
  const actual = await scrypt(password, salt, 32, COST);
  return timingSafeEqual(actual, expected);
}

module.exports = { validPassword, hashPassword, verifyPassword };
