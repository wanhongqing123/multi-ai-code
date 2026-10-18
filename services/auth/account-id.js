'use strict';

const { randomBytes } = require('node:crypto');

const ALPHABET = '0123456789abcdefghjkmnpqrstvwxyz';

function validAccountId(value) {
  return typeof value === 'string' && /^[a-z][a-z0-9_-]{2,31}$/.test(value) &&
    value !== 'administrator';
}

function newAccountId(db) {
  for (let attempt = 0; attempt < 10; attempt++) {
    let value = BigInt(`0x${randomBytes(12).toString('hex')}`);
    let id = '';
    for (let index = 0; index < 20; index++) {
      id = ALPHABET[Number(value & 31n)] + id;
      value >>= 5n;
    }
    id = `m${id}`;
    if (!db.prepare(`SELECT 1 FROM users WHERE id=? OR account_id=? OR im_user_id=?`)
      .get(id, id, id)) return id;
  }
  throw new Error('Could not allocate MaiChat account ID');
}

module.exports = { validAccountId, newAccountId };
