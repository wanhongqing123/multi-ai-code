import { describe, expect, it } from 'vitest'
import { existsSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from 'fs'
import { tmpdir } from 'os'
import { join } from 'path'
import { seedAccountCodexConfig, withCodexAccountHome } from '../../../electron/aicli/codexRuntime.js'

function withTempHome(body: (home: string) => void): void {
  const root = mkdtempSync(join(tmpdir(), 'codex-home-'))
  try {
    body(join(root, '.codex'))
  } finally {
    rmSync(root, { recursive: true, force: true })
  }
}

// 账号隔离之后，「Windows 沙箱已安装完成」的标记留在旧的全局目录里，新账号目录看不到它。
// 沙箱级别是 elevated 时 codex 会认为需要重装，去拉 codex-windows-sandbox-setup.exe——
// 那个 helper 当前不打包，于是弹「Windows 找不到文件」。用户明确要求不使用沙箱，
// 而这件事不该丢给用户自己去手写配置。
describe('seedAccountCodexConfig', () => {
  it('writes a visible disabled-sandbox default when the account home is new', () => {
    withTempHome((home) => {
      withCodexAccountHome({}, home)
      const text = readFileSync(join(home, 'config.toml'), 'utf8')
      expect(text).toContain('[windows]')
      expect(text).toContain('sandbox = "disabled"')
      // 降低隔离强度的默认值必须看得见、改得回，所以注释里要写清另外两个取值。
      expect(text).toContain('unelevated')
      expect(text).toContain('elevated')
    })
  })

  // 覆盖用户已有配置才是真正的「静默削弱隔离」，这条是这个功能的红线。
  it('never overwrites an existing config, whatever it says', () => {
    withTempHome((home) => {
      withCodexAccountHome({}, home)
      const file = join(home, 'config.toml')
      const chosen = '[windows]\nsandbox = "elevated"\n'
      writeFileSync(file, chosen, 'utf8')
      withCodexAccountHome({}, home)
      expect(readFileSync(file, 'utf8')).toBe(chosen)
    })
  })

  // 用户可能故意清空它来回到 codex 的内置默认值；再次启动不该把默认值塞回去。
  it('leaves an intentionally emptied config empty', () => {
    withTempHome((home) => {
      withCodexAccountHome({}, home)
      const file = join(home, 'config.toml')
      writeFileSync(file, '', 'utf8')
      seedAccountCodexConfig(home)
      expect(readFileSync(file, 'utf8')).toBe('')
    })
  })

  // 写不下去（只读目录、磁盘满）不该让会话起不来——codex 有自己的内置默认值。
  it('stays non-fatal when the config cannot be written', () => {
    withTempHome((home) => {
      writeFileSync(home.replace(/[\\/]\.codex$/, '/blocker'), 'x', 'utf8')
      expect(() => seedAccountCodexConfig(join(home, 'missing-parent'))).not.toThrow()
      expect(existsSync(join(home, 'missing-parent', 'config.toml'))).toBe(false)
    })
  })
})

describe('withCodexAccountHome', () => {
  it('refuses a relative home so nothing lands inside the target repo', () => {
    expect(() => withCodexAccountHome({}, 'relative/.codex')).toThrow(/absolute/)
  })

  // Windows 的环境变量名不区分大小写，但 {...process.env} 保留的是继承时的原始拼写。
  // 不先删别名的话，对象里会同时存在两个键，交给 CreateProcess 后谁胜出不确定。
  it('drops inherited aliases in any casing before injecting the account home', () => {
    withTempHome((home) => {
      const env = withCodexAccountHome(
        { Codex_Home: 'X:/inherited', codex_sqlite_home: 'Y:/inherited', PATH: 'keep' },
        home
      )
      expect(Object.keys(env).filter((k) => k.toUpperCase() === 'CODEX_HOME')).toEqual(['CODEX_HOME'])
      expect(env.CODEX_HOME).toBe(home)
      expect(env.CODEX_SQLITE_HOME).toBe(home)
      expect(env.PATH).toBe('keep')
    })
  })

  it('clears a developer capture sink but keeps managed policy and CA settings', () => {
    withTempHome((home) => {
      const env = withCodexAccountHome(
        {
          CODEX_ANALYTICS_EVENTS_CAPTURE_FILE: 'X:/capture.jsonl',
          CODEX_CA_CERTIFICATE: 'X:/ca.pem',
          CODEX_APP_SERVER_MANAGED_CONFIG_PATH: 'X:/managed.toml'
        },
        home
      )
      expect(env).not.toHaveProperty('CODEX_ANALYTICS_EVENTS_CAPTURE_FILE')
      expect(env.CODEX_CA_CERTIFICATE).toBe('X:/ca.pem')
      expect(env.CODEX_APP_SERVER_MANAGED_CONFIG_PATH).toBe('X:/managed.toml')
    })
  })
})
