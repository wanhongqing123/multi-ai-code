import { spawn as spawnChild } from 'child_process'
import { app } from 'electron'
import { access } from 'fs/promises'
import { constants as fsConstants } from 'fs'
import { join } from 'path'

export interface ManagedChromeState {
  running: boolean
  port: number | null
  profileDir: string | null
  pid: number | null
  lastActiveUrl: string | null
}

interface FindChromeExecutableDeps {
  platform?: NodeJS.Platform
  env?: NodeJS.ProcessEnv
  exists?: (target: string) => Promise<boolean>
}

interface ManagedChromeDeps {
  spawn?: (
    command: string,
    args: readonly string[],
    options: ManagedChromeSpawnOptions
  ) => ManagedChromeChild
  resolveChromePath?: () => Promise<string>
  getPort?: () => number
  getProfileDir?: () => string
}

const DEFAULT_MANAGED_CHROME_PORT = 9222

const EMPTY_STATE: ManagedChromeState = {
  running: false,
  port: null,
  profileDir: null,
  pid: null,
  lastActiveUrl: null
}

interface ManagedChromeSpawnOptions {
  detached: boolean
  shell: boolean
  stdio: 'ignore'
  windowsHide: boolean
}

interface ManagedChromeChild {
  pid?: number
  kill(signal?: NodeJS.Signals | number): boolean
  once(
    event: 'exit',
    listener: (code: number | null, signal: NodeJS.Signals | null) => void
  ): this
}

async function pathExists(target: string): Promise<boolean> {
  try {
    await access(target, fsConstants.R_OK)
    return true
  } catch {
    return false
  }
}

function chromeCandidates(
  platform: NodeJS.Platform,
  env: NodeJS.ProcessEnv
): string[] {
  if (platform === 'win32') {
    return [
      env.LOCALAPPDATA && join(env.LOCALAPPDATA, 'Google', 'Chrome', 'Application', 'chrome.exe'),
      env.PROGRAMFILES && join(env.PROGRAMFILES, 'Google', 'Chrome', 'Application', 'chrome.exe'),
      env['PROGRAMFILES(X86)'] &&
        join(env['PROGRAMFILES(X86)'], 'Google', 'Chrome', 'Application', 'chrome.exe')
    ].filter((value): value is string => Boolean(value))
  }

  if (platform === 'darwin') {
    return [
      '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome',
      join(
        env.HOME ?? '',
        'Applications',
        'Google Chrome.app',
        'Contents',
        'MacOS',
        'Google Chrome'
      )
    ].filter(Boolean)
  }

  return [
    '/usr/bin/google-chrome',
    '/usr/bin/google-chrome-stable',
    '/usr/bin/chromium',
    '/usr/bin/chromium-browser'
  ]
}

export async function findChromeExecutable(
  deps: FindChromeExecutableDeps = {}
): Promise<string> {
  const platform = deps.platform ?? process.platform
  const env = deps.env ?? process.env
  const exists = deps.exists ?? pathExists
  for (const candidate of chromeCandidates(platform, env)) {
    if (await exists(candidate)) return candidate
  }
  throw new Error('Chrome executable not found')
}

export interface ManagedChromeManager {
  start(): Promise<ManagedChromeState>
  stop(): Promise<void>
  getState(): ManagedChromeState
  focus(): Promise<void>
}

export function createManagedChromeManager(
  deps: ManagedChromeDeps = {}
): ManagedChromeManager {
  const spawn = deps.spawn ?? spawnChild
  const resolveChromePath = deps.resolveChromePath ?? (() => findChromeExecutable())
  const getPort = deps.getPort ?? (() => DEFAULT_MANAGED_CHROME_PORT)
  const getProfileDir =
    deps.getProfileDir ??
    (() => join(app.getPath('userData'), 'managed-chrome-profile'))

  let state: ManagedChromeState = { ...EMPTY_STATE }
  let child: ManagedChromeChild | null = null

  function resetState(): void {
    state = { ...EMPTY_STATE }
    child = null
  }

  function bindChild(nextChild: ManagedChromeChild, port: number, profileDir: string): void {
    child = nextChild
    state = {
      running: true,
      port,
      profileDir,
      pid: nextChild.pid ?? null,
      lastActiveUrl: null
    }

    nextChild.once('exit', () => {
      resetState()
    })
  }

  return {
    async start(): Promise<ManagedChromeState> {
      if (state.running) return { ...state }

      const chromePath = await resolveChromePath()
      const port = getPort()
      const profileDir = getProfileDir()
      const args = [
        `--remote-debugging-port=${port}`,
        `--user-data-dir=${profileDir}`,
        '--no-first-run',
        '--no-default-browser-check'
      ]
      const nextChild = spawn(chromePath, args, {
        detached: false,
        shell: false,
        stdio: 'ignore',
        windowsHide: true
      })

      bindChild(nextChild, port, profileDir)
      return { ...state }
    },

    async stop(): Promise<void> {
      const activeChild = child
      resetState()
      if (activeChild) {
        activeChild.kill()
      }
    },

    getState(): ManagedChromeState {
      return { ...state }
    },

    async focus(): Promise<void> {
      if (!state.running) {
        await this.start()
      }
    }
  }
}
