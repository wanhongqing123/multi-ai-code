import { describe, expect, it } from 'vitest'
import { spawnSync } from 'child_process'
import { existsSync, mkdtempSync, readdirSync, rmSync, writeFileSync } from 'fs'
import { tmpdir } from 'os'
import { join } from 'path'
import { withCodexHome } from '../../../electron/aicli/codexRuntime.js'

function withTempHome(body: (home: string) => void): void {
  const root = mkdtempSync(join(tmpdir(), 'codex-home-'))
  try {
    body(join(root, '.codex'))
  } finally {
    rmSync(root, { recursive: true, force: true })
  }
}

describe('withCodexHome', () => {
  it('refuses a relative home so nothing lands inside the target repo', () => {
    expect(() => withCodexHome({}, 'relative/.codex', process.cwd())).toThrow(/absolute/)
  })

  // Windows 的环境变量名不区分大小写，但 {...process.env} 保留的是继承时的原始拼写。
  // 不先删别名的话，对象里会同时存在两个键，交给 CreateProcess 后谁胜出不确定。
  it('drops inherited aliases in any casing before injecting the shared home', () => {
    withTempHome((home) => {
      const env = withCodexHome(
        { Codex_Home: 'X:/inherited', codex_sqlite_home: 'Y:/inherited', PATH: 'keep' },
        home,
        join(home, '..', 'repo')
      )
      expect(Object.keys(env).filter((k) => k.toUpperCase() === 'CODEX_HOME')).toEqual(['CODEX_HOME'])
      expect(env.CODEX_HOME).toBe(home)
      expect(env.CODEX_SQLITE_HOME).toBe(home)
      expect(env.PATH).toBe('keep')
    })
  })

  it('clears a developer capture sink but keeps managed policy and CA settings', () => {
    withTempHome((home) => {
      const env = withCodexHome(
        {
          CODEX_ANALYTICS_EVENTS_CAPTURE_FILE: 'X:/capture.jsonl',
          CODEX_CA_CERTIFICATE: 'X:/ca.pem',
          CODEX_APP_SERVER_MANAGED_CONFIG_PATH: 'X:/managed.toml'
        },
        home,
        join(home, '..', 'repo')
      )
      expect(env).not.toHaveProperty('CODEX_ANALYTICS_EVENTS_CAPTURE_FILE')
      expect(env.CODEX_CA_CERTIFICATE).toBe('X:/ca.pem')
      expect(env.CODEX_APP_SERVER_MANAGED_CONFIG_PATH).toBe('X:/managed.toml')
    })
  })

  // 我们**不往共享目录写任何 config.toml**。曾经写过一份 `[windows] sandbox = "disabled"`
  // 的默认值，那是错的：内核的 WindowsSandboxModeToml 只有 elevated / unelevated 两个变体，
  // 写 "disabled" 会让整份配置解析失败、codex 直接起不来（见下面的内核回归）。
  // 「关闭沙箱」在内核里的正确表达是**不写这个键**——缺省即 WindowsSandboxLevel::Disabled。
  it('creates the shared home without planting any config file in it', () => {
    withTempHome((home) => {
      withCodexHome({}, home, join(home, '..', 'repo'))
      expect(existsSync(home)).toBe(true)
      expect(readdirSync(home)).toEqual([])
    })
  })
})

// 这条用**打包进安装包的真实内核**跑，不是断言字符串。上一版的缺陷正是「单测只比对
// 文件内容、从没让 codex 读过它」——内核根本不接受那个值，而测试全绿。
const packagedCodex = join(
  process.cwd(),
  'release/win-unpacked/resources/app.asar.unpacked/bin/aicli/codex/win32-x64/codex.exe'
)
const kernelAvailable = process.platform === 'win32' && existsSync(packagedCodex)

describe.runIf(kernelAvailable)('bundled codex kernel: what it accepts for [windows] sandbox', () => {
  function loadConfig(contents: string | null): string {
    const root = mkdtempSync(join(tmpdir(), 'codex-kernel-'))
    try {
      if (contents !== null) writeFileSync(join(root, 'config.toml'), contents, 'utf8')
      // `login status` 未登录时返回非零，所以不能用 execFileSync（它会抛掉输出）。
      // 我们要的是内核对配置的判读，退出码在这里没有意义。
      const run = spawnSync(packagedCodex, ['login', 'status'], {
        env: { ...process.env, CODEX_HOME: root, CODEX_SQLITE_HOME: root },
        encoding: 'utf8',
        timeout: 120_000
      })
      return `${run.stdout ?? ''}${run.stderr ?? ''}`
    } finally {
      rmSync(root, { recursive: true, force: true })
    }
  }

  it('rejects sandbox = "disabled" outright, so we must never write it', () => {
    expect(loadConfig('[windows]\nsandbox = "disabled"\n')).toMatch(
      /unknown variant `disabled`, expected `elevated` or `unelevated`/
    )
  })

  it('loads a home with no config at all, which is how the sandbox stays off', () => {
    expect(loadConfig(null)).not.toMatch(/Error loading configuration/)
  })
})
