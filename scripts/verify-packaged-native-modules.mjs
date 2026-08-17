import { existsSync } from 'node:fs'
import { join, resolve } from 'node:path'
import { pathToFileURL } from 'node:url'
import { spawnSync } from 'node:child_process'

const SMOKE_SCRIPT = String.raw`
const path = require('node:path')
const root = process.env.MULTI_AI_CODE_PACKAGED_ASAR_MODULE_ROOT
if (!root) throw new Error('missing packaged asar module root')
const Database = require(path.join(root, 'better-sqlite3'))
const db = new Database(':memory:')
const sqliteVersion = db.prepare('select sqlite_version() as version').get().version
db.close()
const pty = require(path.join(root, 'node-pty'))
if (typeof pty.spawn !== 'function') throw new Error('node-pty spawn export is missing')
process.stdout.write(JSON.stringify({ abi: process.versions.modules, sqliteVersion }))
`

export function verifyPackagedMacNativeModules(
  appBundle,
  { spawn = spawnSync, environment = process.env } = {}
) {
  const absoluteAppBundle = resolve(appBundle)
  const executable = join(absoluteAppBundle, 'Contents', 'MacOS', 'Multi-AI Code')
  const resources = join(absoluteAppBundle, 'Contents', 'Resources')
  const asarModuleRoot = join(resources, 'app.asar', 'node_modules')
  const nativeRoot = join(
    resources,
    'app.asar.unpacked',
    'node_modules'
  )
  const requiredPaths = [
    executable,
    join(resources, 'app.asar'),
    join(nativeRoot, 'better-sqlite3', 'build', 'Release', 'better_sqlite3.node'),
    join(nativeRoot, 'node-pty', 'build', 'Release', 'pty.node')
  ]
  for (const requiredPath of requiredPaths) {
    if (!existsSync(requiredPath)) {
      throw new Error(`Packaged native module verification path is missing: ${requiredPath}`)
    }
  }

  const result = spawn(executable, ['-e', SMOKE_SCRIPT], {
    encoding: 'utf8',
    env: {
      ...environment,
      ELECTRON_RUN_AS_NODE: '1',
      MULTI_AI_CODE_PACKAGED_ASAR_MODULE_ROOT: asarModuleRoot
    }
  })
  if (result.error) throw result.error
  if (result.status !== 0) {
    const detail = `${result.stderr ?? ''}\n${result.stdout ?? ''}`.trim()
    throw new Error(
      `Packaged native modules failed to load in Electron (exit ${result.status}): ${detail}`
    )
  }

  let summary
  try {
    summary = JSON.parse((result.stdout ?? '').trim())
  } catch {
    throw new Error(`Packaged native module smoke test returned invalid output: ${result.stdout}`)
  }
  const expectedAbi = environment.MULTI_AI_CODE_EXPECTED_ELECTRON_ABI
  if (expectedAbi && summary.abi !== expectedAbi) {
    throw new Error(
      `Packaged Electron ABI mismatch: expected ${expectedAbi}, received ${summary.abi}`
    )
  }
  return summary
}

const invokedPath = process.argv[1] ? pathToFileURL(process.argv[1]).href : ''
if (invokedPath === import.meta.url) {
  const appBundle = process.argv[2]
  if (!appBundle) {
    console.error('Usage: node scripts/verify-packaged-native-modules.mjs <app-bundle>')
    process.exit(2)
  }
  try {
    const summary = verifyPackagedMacNativeModules(appBundle)
    console.log(
      `[package] Electron ABI ${summary.abi}; better-sqlite3 ${summary.sqliteVersion}; node-pty loaded`
    )
  } catch (error) {
    console.error(error instanceof Error ? error.message : String(error))
    process.exit(1)
  }
}
