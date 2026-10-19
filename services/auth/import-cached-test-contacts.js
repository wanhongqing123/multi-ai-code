// One-time, administrator-reviewed migration of locally cached IM contacts.
'use strict';

const fs = require('node:fs');
const Database = require('better-sqlite3');
const { openStore } = require('./store');
const { validAccountId } = require('./account-id');
const { hashImportedTestPassword } = require('./password');
const { createTencentAccountProvisioner } = require('./tim-account-provision');

function validateManifest(manifest) {
  if (!manifest || !validAccountId(manifest.owner_account_id) ||
      typeof manifest.owner_im_user_id !== 'string' ||
      !Array.isArray(manifest.contacts) || manifest.contacts.length < 1 ||
      manifest.contacts.length > 100) throw new Error('Invalid cached contact manifest');
  const seen = new Set();
  for (const contact of manifest.contacts) {
    const id = contact?.im_user_id;
    if (!validAccountId(id) || id === manifest.owner_im_user_id || seen.has(id) ||
        !['Imported', 'NotImported'].includes(contact.expected_status))
      throw new Error('Invalid cached contact entry');
    seen.add(id);
    const nickname = `测试-${id}`;
    if ([...nickname].length > 40 || Buffer.byteLength(nickname, 'utf8') > 120)
      throw new Error('Cached contact nickname is too long');
  }
}

function ownerForManifest(db, manifest) {
  const owner = db.prepare(`SELECT id,account_id,im_user_id,disabled_at FROM users
    WHERE account_id=?`).get(manifest.owner_account_id);
  if (!owner || owner.disabled_at || owner.im_user_id !== manifest.owner_im_user_id)
    throw new Error('Owner account does not match the manifest');
  const marker = `cached_test_contacts_20261019_${owner.account_id}`;
  if (db.prepare('SELECT 1 FROM account_migrations WHERE name=?').get(marker))
    throw new Error('Cached contacts were already imported');
  return { owner, marker };
}

function requireAvailableIds(db, contacts) {
  const lookup = db.prepare('SELECT 1 FROM users WHERE id=? OR account_id=? OR im_user_id=?');
  for (const contact of contacts) {
    const id = contact.im_user_id;
    if (lookup.get(id, id, id)) throw new Error('Cached contact already has a MaiChat account');
  }
}

async function importCachedTestContacts(db, manifest, options) {
  validateManifest(manifest);
  const { owner, marker } = ownerForManifest(db, manifest);
  requireAvailableIds(db, manifest.contacts);
  let existingIm = 0;
  let newIm = 0;
  for (const contact of manifest.contacts) {
    const status = await options.imStatus(contact.im_user_id);
    if (status !== contact.expected_status) throw new Error('IM account status changed');
    if (status === 'Imported') existingIm++;
    else newIm++;
  }
  const summary = { contacts: manifest.contacts.length, existing_im: existingIm,
    new_im: newIm, friendships: manifest.contacts.length };
  if (options.dryRun) return { mode: 'dry-run', ...summary };
  if (typeof options.password !== 'string' || typeof options.importUnused !== 'function')
    throw new Error('Import password or IM provisioner is unavailable');
  const rows = [];
  for (const contact of manifest.contacts) {
    rows.push({ id: contact.im_user_id, nickname: `测试-${contact.im_user_id}`,
      passwordHash: await hashImportedTestPassword(options.password) });
  }
  for (const contact of manifest.contacts) {
    if (contact.expected_status !== 'NotImported') continue;
    if (!await options.importUnused(contact.im_user_id))
      throw new Error('Unused IM account could not be imported');
  }
  const now = options.now ? options.now() : Date.now();
  db.transaction(() => {
    ownerForManifest(db, manifest);
    requireAvailableIds(db, manifest.contacts);
    const insertUser = db.prepare(`INSERT INTO users
      (id,account_id,nickname,phone_e164,im_user_id,password_hash,created_at,updated_at)
      VALUES(?,?,?,NULL,?,?,?,?)`);
    const insertFriend = db.prepare(`INSERT INTO friendships
      (left_user_id,right_user_id,created_at) VALUES(?,?,?)`);
    const queueFriend = db.prepare(`INSERT INTO friend_im_sync
      (left_user_id,right_user_id,desired,updated_at) VALUES(?,?,'add',?)`);
    const queueProfile = db.prepare(`INSERT INTO profile_im_sync
      (user_id,desired_nickname,updated_at) VALUES(?,?,?)`);
    const audit = db.prepare(`INSERT INTO admin_audit
      (action,target_user_id,created_at) VALUES(?,?,?)`);
    for (const row of rows) {
      insertUser.run(row.id, row.id, row.nickname, row.id, row.passwordHash, now, now);
      const [left, right] = [owner.id, row.id].sort();
      insertFriend.run(left, right, now);
      queueFriend.run(left, right, now);
      queueProfile.run(row.id, row.nickname, now);
      audit.run('import_cached_test_account', row.id, now);
    }
    db.prepare('INSERT INTO account_migrations(name,applied_at) VALUES(?,?)')
      .run(marker, now);
  })();
  return { mode: 'run', ...summary };
}

async function main() {
  const mode = process.argv[2];
  if (!['--dry-run', '--run'].includes(mode) || process.argv.length !== 3 ||
      !process.env.MAICHAT_AUTH_DB_PATH || !process.env.MAICHAT_IMPORT_MANIFEST)
    throw new Error('Usage: MAICHAT_IMPORT_MANIFEST=... node import-cached-test-contacts.js --dry-run|--run');
  const manifest = JSON.parse(fs.readFileSync(process.env.MAICHAT_IMPORT_MANIFEST, 'utf8'));
  const provision = createTencentAccountProvisioner({
    sdkAppId: Number(process.env.MAICHAT_IM_SDK_APP_ID),
    imSecret: process.env.MAICHAT_IM_SECRET_KEY,
    adminUser: process.env.MAICHAT_IM_ADMIN_USER || 'administrator',
    origin: process.env.MAICHAT_IM_REST_ORIGIN || 'https://console.tim.qq.com'
  });
  const db = mode === '--run' ? openStore(process.env.MAICHAT_AUTH_DB_PATH) :
    new Database(process.env.MAICHAT_AUTH_DB_PATH, { readonly: true });
  try {
    const result = await importCachedTestContacts(db, manifest, {
      dryRun: mode === '--dry-run', imStatus: provision.checkStatus,
      importUnused: provision, password: process.env.MAICHAT_IMPORT_PASSWORD
    });
    process.stdout.write(`${JSON.stringify(result)}\n`);
  } finally { db.close(); }
}

if (require.main === module) main().catch(error => {
  process.stderr.write(`${error.message}\n`);
  process.exitCode = 1;
});

module.exports = { importCachedTestContacts };
