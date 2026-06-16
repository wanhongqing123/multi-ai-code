import { EventEmitter } from 'events'
import { PassThrough } from 'stream'
import { afterEach, describe, expect, it, vi } from 'vitest'
import { createRuntimeRunner, type SpawnedRuntimeProcess } from './runner.js'
import type { ProjectRuntimeConfig } from './config.js'

class FakeChild extends EventEmitter implements SpawnedRuntimeProcess {
  pid = 4321
  stdout = new PassThrough()
  stderr = new PassThrough()
  kill = vi.fn((signal?: NodeJS.Signals | number) => {
    this.emit('killed', signal)
    return true
  })
}

function flush(): Promise<void> {
  return new Promise((resolve) => setImmediate(resolve))
}

const msysConfig: ProjectRuntimeConfig = {
  enabled: true,
  cwd: '.',
  command: 'npm run dev',
  envType: 'msys',
  visualStudioInstanceId: '',
  outputEncoding: 'auto',
}

afterEach(() => {
  vi.restoreAllMocks()
})

describe('createRuntimeRunner', () => {
  it('starts an msys runtime, captures logs, and transitions to exited on code zero', async () => {
    const child = new FakeChild()
    const dataEvents: string[] = []
    const statuses: string[] = []
    const spawn = vi.fn(() => child)
    const runner = createRuntimeRunner({
      platform: 'win32',
      spawn,
      detectMsys: vi.fn().mockResolvedValue({
        available: true,
        bashPath: 'C:\\msys64\\usr\\bin\\bash.exe',
        usrBinDir: 'C:\\msys64\\usr\\bin',
        variant: 'msys2',
        candidates: [],
      }),
      resolveVisualStudioEnvironment: vi.fn(),
      now: () => '2026-06-12T10:00:00.000Z',
    })
    runner.onData((event) => dataEvents.push(`${event.stream}:${event.chunk}`))
    runner.onStatus((state) => statuses.push(state.status))

    const start = await runner.start({
      projectId: 'p1',
      projectName: 'Demo',
      targetRepo: 'E:\\repo',
      config: msysConfig,
    })

    expect(start.ok).toBe(true)
    expect(spawn).toHaveBeenCalledWith(
      'C:\\msys64\\usr\\bin\\bash.exe',
      ['-lc', "cd '/e/repo' && npm run dev"],
      expect.objectContaining({
        cwd: 'E:\\repo',
        shell: false,
        windowsHide: true,
      })
    )
    expect(runner.getState()).toMatchObject({
      status: 'running',
      projectId: 'p1',
      projectName: 'Demo',
      cwd: 'E:\\repo',
      command: 'npm run dev',
      envType: 'msys',
    })

    child.stdout.write('server started\n')
    child.stderr.write('warn once\n')
    child.emit('close', 0, null)
    await flush()

    expect(runner.getState()).toMatchObject({
      status: 'exited',
      exitCode: 0,
      signal: null,
      finishedAt: '2026-06-12T10:00:00.000Z',
    })
    expect(runner.getState().log).toContain('server started')
    expect(runner.getState().log).toContain('warn once')
    expect(dataEvents).toContain('stdout:server started\n')
    expect(dataEvents).toContain('stderr:warn once\n')
    expect(statuses).toContain('running')
    expect(statuses).toContain('exited')
  })

  it('prefers a PTY runtime process when available so console apps stream live output', async () => {
    const pipeChild = new FakeChild()
    const ptyChild = new FakeChild()
    const spawn = vi.fn(() => pipeChild)
    const spawnPty = vi.fn(() => ptyChild)
    const runner = createRuntimeRunner({
      platform: 'win32',
      spawn,
      spawnPty,
      detectMsys: vi.fn().mockResolvedValue({
        available: true,
        bashPath: 'C:\\msys64\\usr\\bin\\bash.exe',
        usrBinDir: 'C:\\msys64\\usr\\bin',
        variant: 'msys2',
        candidates: [],
      }),
      resolveVisualStudioEnvironment: vi.fn(),
      now: () => '2026-06-12T10:00:00.000Z',
    })

    await runner.start({
      projectId: 'p1',
      projectName: 'Demo',
      targetRepo: 'E:\\repo',
      config: msysConfig,
    })

    expect(spawnPty).toHaveBeenCalledWith(
      'C:\\msys64\\usr\\bin\\bash.exe',
      ['-lc', "cd '/e/repo' && npm run dev"],
      expect.objectContaining({
        cwd: 'E:\\repo',
      })
    )
    expect(spawn).not.toHaveBeenCalled()

    ptyChild.stdout.write('demo tick\n')
    await flush()

    expect(runner.getState().log).toContain('demo tick')
  })

  it('blocks disabled runtime configs and empty commands before spawning', async () => {
    const spawn = vi.fn()
    const runner = createRuntimeRunner({
      platform: 'win32',
      spawn,
      detectMsys: vi.fn(),
      resolveVisualStudioEnvironment: vi.fn(),
      now: () => '2026-06-12T10:00:00.000Z',
    })

    const disabled = await runner.start({
      projectId: 'p-disabled',
      projectName: 'Demo',
      targetRepo: 'E:\\repo',
      config: { ...msysConfig, enabled: false },
    })
    expect(disabled).toMatchObject({
      ok: false,
      error: 'runtime is disabled',
      state: { status: 'idle' },
    })

    const empty = await runner.start({
      projectId: 'p-empty',
      projectName: 'Demo',
      targetRepo: 'E:\\repo',
      config: { ...msysConfig, command: '   ' },
    })
    expect(empty).toMatchObject({
      ok: false,
      error: 'runtime command is empty',
      state: { status: 'idle' },
    })
    expect(spawn).not.toHaveBeenCalled()
  })

  it('kills the active runtime process tree on Windows and transitions to stopped', async () => {
    const child = new FakeChild()
    const killProcessTree = vi.fn()
    const runner = createRuntimeRunner({
      platform: 'win32',
      spawn: vi.fn(() => child),
      killProcessTree,
      detectMsys: vi.fn().mockResolvedValue({
        available: true,
        bashPath: 'C:\\msys64\\usr\\bin\\bash.exe',
        usrBinDir: 'C:\\msys64\\usr\\bin',
        variant: 'msys2',
        candidates: [],
      }),
      resolveVisualStudioEnvironment: vi.fn(),
      now: () => '2026-06-12T10:00:00.000Z',
    })

    await runner.start({
      projectId: 'p-stop',
      projectName: 'Demo',
      targetRepo: 'E:\\repo',
      config: msysConfig,
    })

    expect(runner.stop()).toEqual({ ok: true })
    expect(killProcessTree).toHaveBeenCalledWith(4321)
    expect(child.kill).not.toHaveBeenCalled()

    child.emit('close', null, 'SIGTERM')
    await flush()

    expect(runner.getState()).toMatchObject({
      status: 'stopped',
      signal: 'SIGTERM',
    })
    expect(runner.getState().log).toContain('[runtime] stop requested')
  })

  it('spawns through Visual Studio environment and decodes auto output as gbk', async () => {
    const child = new FakeChild()
    const resolveVisualStudioEnvironment = vi.fn().mockResolvedValue({
      ok: true,
      displayName: 'Visual Studio 2022',
      installationPath: 'C:\\VS',
      devCmdPath: 'C:\\VS\\Common7\\Tools\\VsDevCmd.bat',
      env: { Path: 'C:\\VS\\bin' },
    })
    const spawn = vi.fn(() => child)
    const runner = createRuntimeRunner({
      platform: 'win32',
      spawn,
      detectMsys: vi.fn(),
      resolveVisualStudioEnvironment,
      now: () => '2026-06-12T10:00:00.000Z',
    })

    await runner.start({
      projectId: 'p-vs',
      projectName: 'Demo',
      targetRepo: 'E:\\repo',
      config: {
        enabled: true,
        cwd: 'app',
        command: 'demo.exe',
        envType: 'visual-studio',
        visualStudioInstanceId: 'vs-1',
        outputEncoding: 'auto',
      },
    })

    expect(resolveVisualStudioEnvironment).toHaveBeenCalledWith({
      instanceId: 'vs-1',
      platform: 'win32',
      baseEnv: process.env,
    })
    expect(spawn).toHaveBeenCalledWith(
      'cmd.exe',
      ['/d', '/s', '/c', 'demo.exe'],
      expect.objectContaining({
        cwd: 'E:\\repo\\app',
        env: { Path: 'C:\\VS\\bin' },
        windowsVerbatimArguments: true,
      })
    )

    child.stdout.write(Buffer.from([0xd6, 0xd0, 0xce, 0xc4, 0x0a]))
    child.emit('close', 2, null)
    await flush()

    expect(runner.getState()).toMatchObject({
      status: 'failed',
      exitCode: 2,
      visualStudioDisplayName: 'Visual Studio 2022',
    })
    expect(runner.getState().log).toContain('\u4e2d\u6587')
  })

  it('truncates the runtime log with a visible marker once the limit is exceeded', async () => {
    const child = new FakeChild()
    const runner = createRuntimeRunner({
      platform: 'win32',
      spawn: vi.fn(() => child),
      detectMsys: vi.fn().mockResolvedValue({
        available: true,
        bashPath: 'C:\\msys64\\usr\\bin\\bash.exe',
        usrBinDir: 'C:\\msys64\\usr\\bin',
        variant: 'msys2',
        candidates: [],
      }),
      resolveVisualStudioEnvironment: vi.fn(),
      now: () => '2026-06-12T10:00:00.000Z',
      logLimit: 80,
    })

    await runner.start({
      projectId: 'p-log',
      projectName: 'Demo',
      targetRepo: 'E:\\repo',
      config: msysConfig,
    })

    child.stdout.write('1234567890abcdefghij1234567890abcdefghij1234567890abcdefghij1234567890\n')
    child.stderr.write('tail-marker\n')
    child.emit('close', 0, null)
    await flush()

    const log = runner.getState().log
    expect(log).toContain('[runtime] earlier log truncated...')
    expect(log).toContain('tail-marker')
    expect(log.length).toBeLessThanOrEqual(80)
  })

  it('rejects cwd values that escape targetRepo before spawning', async () => {
    const spawn = vi.fn()
    const runner = createRuntimeRunner({
      platform: 'win32',
      spawn,
      detectMsys: vi.fn(),
      resolveVisualStudioEnvironment: vi.fn(),
      now: () => '2026-06-12T10:00:00.000Z',
    })

    const result = await runner.start({
      projectId: 'p-escape',
      projectName: 'Demo',
      targetRepo: 'E:\\repo',
      config: { ...msysConfig, cwd: '..\\outside' },
    })

    expect(result).toMatchObject({
      ok: false,
      error: 'cwd must stay within target_repo',
      state: { status: 'idle' },
    })
    expect(spawn).not.toHaveBeenCalled()
  })
})
