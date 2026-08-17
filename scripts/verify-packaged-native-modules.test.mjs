import { mkdirSync, mkdtempSync, rmSync, writeFileSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { describe, expect, it } from 'vitest'
import { verifyPackagedMacNativeModules } from './verify-packaged-native-modules.mjs'

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
