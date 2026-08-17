import { existsSync } from 'node:fs'
import { join, resolve } from 'node:path'
import { pathToFileURL } from 'node:url'
import { spawnSync } from 'node:child_process'

/**
 * 在打包产物里真正加载原生模块，而不是只检查文件在不在。
 *
 * 起因是一次 ABI 不匹配的发布：文件都在、体积也对，装上却起不来。文件存在
 * 证明不了它能被这个 Electron 加载——ABI 对不上时 require 才会炸。所以这里
 * 一律用打包出来的 Electron 自己（ELECTRON_RUN_AS_NODE）去 require 一遍。
 *
 * Windows 比 macOS 多两个原生模块：koffi（远程控制注入用）和 TRTC SDK
 * （远程桌面）。koffi 的二进制还不在自己的包里，而在平台专属的
 * @koromix/koffi-win32-x64 —— 靠人工记得去验不可靠，必须进自动关卡。
 */

const SMOKE_SCRIPT = String.raw`
const path = require('node:path')
const root = process.env.MULTI_AI_CODE_PACKAGED_ASAR_MODULE_ROOT
if (!root) throw new Error('missing packaged asar module root')
const modules = (process.env.MULTI_AI_CODE_PACKAGED_SMOKE_MODULES || '').split(',').filter(Boolean)
const loaded = {}

const Database = require(path.join(root, 'better-sqlite3'))
const db = new Database(':memory:')
const sqliteVersion = db.prepare('select sqlite_version() as version').get().version
db.close()
loaded['better-sqlite3'] = sqliteVersion

const pty = require(path.join(root, 'node-pty'))
if (typeof pty.spawn !== 'function') throw new Error('node-pty spawn export is missing')
loaded['node-pty'] = 'ok'

// koffi 只在 Windows 打包，且必须真调一次系统 API：光 require 成功不代表
// 平台专属的 koffi.node 被正确解析到（它在另一个包里）。
if (modules.includes('koffi')) {
  const koffi = require(path.join(root, 'koffi'))
  const user32 = koffi.load('user32.dll')
  const GetSystemMetrics = user32.func('int __stdcall GetSystemMetrics(int index)')
  const width = GetSystemMetrics(0)
  if (!Number.isInteger(width) || width <= 0) throw new Error('koffi user32 call returned ' + width)
  loaded.koffi = koffi.version
}

// TRTC 的 index.js 在模块级访问 DOM，ELECTRON_RUN_AS_NODE 下 require 会抛
// "document is not defined"。所以只对 .node 本身做 dlopen —— 这已经足以
// 暴露 ABI 不匹配，而那正是这个检查要防的东西。
if (modules.includes('trtc-electron-sdk')) {
  const nodeFile = path.join(
    process.env.MULTI_AI_CODE_PACKAGED_NATIVE_ROOT,
    'trtc-electron-sdk', 'build', 'Release', 'trtc_electron_sdk.node'
  )
  const holder = { exports: {} }
  process.dlopen(holder, nodeFile)
  loaded['trtc-electron-sdk'] = 'ok'
}

process.stdout.write(JSON.stringify({ abi: process.versions.modules, sqliteVersion, loaded }))
`

/** 各平台的打包布局与必须存在的原生二进制。 */
const PLATFORM_LAYOUTS = {
  darwin: {
    executable: (appDir) => join(appDir, 'Contents', 'MacOS', 'Multi-AI Code'),
    resources: (appDir) => join(appDir, 'Contents', 'Resources'),
    // macOS 只打包这两个：koffi 和 TRTC 目前仅用于 Windows 的远程桌面/控制。
    smokeModules: [],
    nativeFiles: [
      ['better-sqlite3', 'build', 'Release', 'better_sqlite3.node'],
      ['node-pty', 'build', 'Release', 'pty.node']
    ]
  },
  win32: {
    executable: (appDir) => join(appDir, 'Multi-AI Code.exe'),
    resources: (appDir) => join(appDir, 'resources'),
    smokeModules: ['koffi', 'trtc-electron-sdk'],
    nativeFiles: [
      ['better-sqlite3', 'build', 'Release', 'better_sqlite3.node'],
      // Windows 上 node-pty 优先加载 build/Release，prebuilds 是回退。
      ['node-pty', 'build', 'Release', 'pty.node'],
      ['node-pty', 'build', 'Release', 'conpty.node'],
      // koffi 的二进制不在 koffi 包里，而在平台专属包。asarUnpack 只写
      // node_modules/koffi/** 是不够的——这一条就是用来盯住它的。
      ['@koromix', 'koffi-win32-x64', 'win32_x64', 'koffi.node'],
      ['trtc-electron-sdk', 'build', 'Release', 'trtc_electron_sdk.node']
    ]
  }
}

export function verifyPackagedNativeModules(
  appDir,
  { platform, spawn = spawnSync, environment = process.env } = {}
) {
  const layout = PLATFORM_LAYOUTS[platform]
  if (!layout) throw new Error(`Unsupported packaged platform: ${platform}`)

  const absoluteAppDir = resolve(appDir)
  const executable = layout.executable(absoluteAppDir)
  const resources = layout.resources(absoluteAppDir)
  const asarModuleRoot = join(resources, 'app.asar', 'node_modules')
  const nativeRoot = join(resources, 'app.asar.unpacked', 'node_modules')

  const requiredPaths = [
    executable,
    join(resources, 'app.asar'),
    ...layout.nativeFiles.map((segments) => join(nativeRoot, ...segments))
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
      MULTI_AI_CODE_PACKAGED_ASAR_MODULE_ROOT: asarModuleRoot,
      MULTI_AI_CODE_PACKAGED_NATIVE_ROOT: nativeRoot,
      MULTI_AI_CODE_PACKAGED_SMOKE_MODULES: layout.smokeModules.join(',')
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
  // 冒烟脚本报告的已加载模块必须覆盖本平台要求的那几个。少了说明脚本被
  // 改动过或环境变量没传到，而不是"这个平台不需要"。
  for (const name of layout.smokeModules) {
    if (!summary.loaded?.[name]) {
      throw new Error(`Packaged native module did not report a successful load: ${name}`)
    }
  }
  return summary
}

/** 保留原名：macOS 打包脚本按这个名字调用。 */
export function verifyPackagedMacNativeModules(appBundle, options = {}) {
  return verifyPackagedNativeModules(appBundle, { ...options, platform: 'darwin' })
}

export function verifyPackagedWindowsNativeModules(appDir, options = {}) {
  return verifyPackagedNativeModules(appDir, { ...options, platform: 'win32' })
}

const invokedPath = process.argv[1] ? pathToFileURL(process.argv[1]).href : ''
if (invokedPath === import.meta.url) {
  const appDir = process.argv[2]
  const platform = process.argv[3] ?? process.platform
  if (!appDir) {
    console.error(
      'Usage: node scripts/verify-packaged-native-modules.mjs <app-dir> [darwin|win32]'
    )
    process.exit(2)
  }
  try {
    const summary = verifyPackagedNativeModules(appDir, { platform })
    const extras = Object.entries(summary.loaded ?? {})
      .map(([name, detail]) => `${name} ${detail}`)
      .join('; ')
    console.log(`[package] Electron ABI ${summary.abi}; ${extras}`)
  } catch (error) {
    console.error(error instanceof Error ? error.message : String(error))
    process.exit(1)
  }
}
