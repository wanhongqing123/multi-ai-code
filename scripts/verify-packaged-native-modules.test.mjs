import { mkdirSync, mkdtempSync, rmSync, writeFileSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { describe, expect, it } from 'vitest'
import {
  verifyPackagedMacNativeModules,
  verifyPackagedNativeModules,
  verifyPackagedWindowsNativeModules
} from './verify-packaged-native-modules.mjs'

function createFakeApp() {
  const root = mkdtempSync(join(tmpdir(), 'packaged-native-'))
  const app = join(root, 'Multi-AI Code.app')
  const executable = join(app, 'Contents', 'MacOS', 'Multi-AI Code')
  const resources = join(app, 'Contents', 'Resources')
  const modules = join(resources, 'app.asar.unpacked', 'node_modules')
  const asarModules = join(resources, 'app.asar', 'node_modules')
  mkdirSync(join(modules, 'better-sqlite3', 'build', 'Release'), { recursive: true })
  mkdirSync(join(modules, 'node-pty', 'build', 'Release'), { recursive: true })
  mkdirSync(join(app, 'Contents', 'MacOS'), { recursive: true })
  writeFileSync(executable, '')
  writeFileSync(join(resources, 'app.asar'), '')
  writeFileSync(join(modules, 'better-sqlite3', 'build', 'Release', 'better_sqlite3.node'), '')
  writeFileSync(join(modules, 'node-pty', 'build', 'Release', 'pty.node'), '')
  return { root, app, executable, asarModules }
}

describe('packaged Electron native module verification', () => {
  it('runs the packaged executable as Electron Node and accepts ABI 130', () => {
    const fixture = createFakeApp()
    const calls = []
    try {
      const summary = verifyPackagedMacNativeModules(fixture.app, {
        environment: {
          PATH: '/usr/bin',
          MULTI_AI_CODE_EXPECTED_ELECTRON_ABI: '130'
        },
        spawn(executable, args, options) {
          calls.push({ executable, args, options })
          return {
            status: 0,
            stdout: JSON.stringify({ abi: '130', sqliteVersion: '3.49.2' }),
            stderr: ''
          }
        }
      })

      expect(summary).toEqual({ abi: '130', sqliteVersion: '3.49.2' })
      expect(calls).toHaveLength(1)
      expect(calls[0].executable).toBe(fixture.executable)
      expect(calls[0].options.env.ELECTRON_RUN_AS_NODE).toBe('1')
      expect(calls[0].options.env.MULTI_AI_CODE_PACKAGED_ASAR_MODULE_ROOT).toBe(
        fixture.asarModules
      )
    } finally {
      rmSync(fixture.root, { recursive: true, force: true })
    }
  })

  it('rejects the host Node ABI that caused the broken release', () => {
    const fixture = createFakeApp()
    try {
      expect(() =>
        verifyPackagedMacNativeModules(fixture.app, {
          environment: { MULTI_AI_CODE_EXPECTED_ELECTRON_ABI: '130' },
          spawn: () => ({
            status: 0,
            stdout: JSON.stringify({ abi: '127', sqliteVersion: '3.49.2' }),
            stderr: ''
          })
        })
      ).toThrow('expected 130, received 127')
    } finally {
      rmSync(fixture.root, { recursive: true, force: true })
    }
  })

  it('surfaces an Electron require failure before publishing', () => {
    const fixture = createFakeApp()
    try {
      expect(() =>
        verifyPackagedMacNativeModules(fixture.app, {
          spawn: () => ({
            status: 1,
            stdout: '',
            stderr: 'NODE_MODULE_VERSION 127; expected 130'
          })
        })
      ).toThrow('NODE_MODULE_VERSION 127')
    } finally {
      rmSync(fixture.root, { recursive: true, force: true })
    }
  })
})


function createFakeWindowsApp({ omit = [] } = {}) {
  const root = mkdtempSync(join(tmpdir(), 'packaged-native-win-'))
  const app = join(root, 'win-unpacked')
  const executable = join(app, 'Multi-AI Code.exe')
  const resources = join(app, 'resources')
  const modules = join(resources, 'app.asar.unpacked', 'node_modules')
  const asarModules = join(resources, 'app.asar', 'node_modules')
  // 路径取自真实打包产物：node-pty 在 Windows 走 build/Release，
  // koffi 的二进制在平台专属包 @koromix/koffi-win32-x64 里。
  const nativeFiles = [
    ['better-sqlite3', 'build', 'Release', 'better_sqlite3.node'],
    ['node-pty', 'build', 'Release', 'pty.node'],
    ['node-pty', 'build', 'Release', 'conpty.node'],
    ['@koromix', 'koffi-win32-x64', 'win32_x64', 'koffi.node'],
    ['trtc-electron-sdk', 'build', 'Release', 'trtc_electron_sdk.node']
  ]
  mkdirSync(app, { recursive: true })
  writeFileSync(executable, '')
  mkdirSync(resources, { recursive: true })
  writeFileSync(join(resources, 'app.asar'), '')
  for (const segments of nativeFiles) {
    const relative = segments.join('/')
    if (omit.includes(relative)) continue
    mkdirSync(join(modules, ...segments.slice(0, -1)), { recursive: true })
    writeFileSync(join(modules, ...segments), '')
  }
  return { root, app, executable, asarModules, modules }
}

const WINDOWS_OK_STDOUT = JSON.stringify({
  abi: '130',
  sqliteVersion: '3.49.2',
  loaded: {
    'better-sqlite3': '3.49.2',
    'node-pty': 'ok',
    koffi: '3.1.5',
    'trtc-electron-sdk': 'ok'
  }
})

describe('packaged Windows native module verification', () => {
  it('runs the packaged exe and reports the Windows-only modules', () => {
    const fixture = createFakeWindowsApp()
    const calls = []
    try {
      const summary = verifyPackagedWindowsNativeModules(fixture.app, {
        environment: { MULTI_AI_CODE_EXPECTED_ELECTRON_ABI: '130' },
        spawn(executable, args, options) {
          calls.push({ executable, args, options })
          return { status: 0, stdout: WINDOWS_OK_STDOUT, stderr: '' }
        }
      })

      expect(summary.loaded.koffi).toBe('3.1.5')
      expect(calls[0].executable).toBe(fixture.executable)
      expect(calls[0].options.env.ELECTRON_RUN_AS_NODE).toBe('1')
      // koffi 和 TRTC 只在 Windows 冒烟，靠这个环境变量告知脚本。
      expect(calls[0].options.env.MULTI_AI_CODE_PACKAGED_SMOKE_MODULES).toBe(
        'koffi,trtc-electron-sdk'
      )
    } finally {
      rmSync(fixture.root, { recursive: true, force: true })
    }
  })

  it('catches a koffi binary that never made it out of the asar', () => {
    // 真实踩过的坑：asarUnpack 只写了 node_modules/koffi/**，而二进制在
    // @koromix/koffi-win32-x64 里。漏了的话远程控制在装机后静默失效。
    const fixture = createFakeWindowsApp({
      omit: ['@koromix/koffi-win32-x64/win32_x64/koffi.node']
    })
    try {
      expect(() =>
        verifyPackagedWindowsNativeModules(fixture.app, { spawn: () => ({ status: 0, stdout: WINDOWS_OK_STDOUT }) })
      ).toThrow('koffi.node')
    } finally {
      rmSync(fixture.root, { recursive: true, force: true })
    }
  })

  it('catches a missing TRTC binary', () => {
    const fixture = createFakeWindowsApp({
      omit: ['trtc-electron-sdk/build/Release/trtc_electron_sdk.node']
    })
    try {
      expect(() =>
        verifyPackagedWindowsNativeModules(fixture.app, { spawn: () => ({ status: 0, stdout: WINDOWS_OK_STDOUT }) })
      ).toThrow('trtc_electron_sdk.node')
    } finally {
      rmSync(fixture.root, { recursive: true, force: true })
    }
  })

  it('rejects a smoke run that silently skipped a Windows-only module', () => {
    // 文件都在、exit 0，但冒烟脚本没报告 koffi —— 说明环境变量没传到或
    // 脚本被改过。这种情况必须当失败，否则关卡形同虚设。
    const fixture = createFakeWindowsApp()
    try {
      expect(() =>
        verifyPackagedWindowsNativeModules(fixture.app, {
          spawn: () => ({
            status: 0,
            stdout: JSON.stringify({ abi: '130', sqliteVersion: '3.49.2', loaded: { 'node-pty': 'ok' } })
          })
        })
      ).toThrow('did not report a successful load: koffi')
    } finally {
      rmSync(fixture.root, { recursive: true, force: true })
    }
  })

  it('rejects an unknown platform instead of silently verifying nothing', () => {
    // 包装函数会把 platform 写死，所以这里直接测底层：新平台没登记布局时
    // 必须报错，不能当成"没有要验的东西"而放行。
    expect(() =>
      verifyPackagedNativeModules('/tmp/whatever', { platform: 'linux' })
    ).toThrow('Unsupported packaged platform')
  })
})
