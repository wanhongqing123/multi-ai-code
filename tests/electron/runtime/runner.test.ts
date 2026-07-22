import { EventEmitter } from 'events'
import { PassThrough } from 'stream'
import { afterEach, describe, expect, it, vi } from 'vitest'
import { createRuntimeRunner, type SpawnedRuntimeProcess } from '../../../electron/runtime/runner.js'
import type { ProjectRuntimeConfig } from '../../../electron/runtime/config.js'

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
  it('starts system runtime commands in the host shell without resolving Windows environments', async () => {
    const child = new FakeChild()
    const spawn = vi.fn(() => child)
    const spawnPty = vi.fn()
    const detectMsys = vi.fn()
    const resolveVisualStudioEnvironment = vi.fn()
    const runner = createRuntimeRunner({
      platform: 'darwin',
      spawn,
      spawnPty,
      detectMsys,
      resolveVisualStudioEnvironment,
      now: () => '2026-06-12T10:00:00.000Z',
    })

    const start = await runner.start({
      projectId: 'p-system',
      projectName: 'Demo',
      targetRepo: '/repo',
      config: {
        ...msysConfig,
        envType: 'system',
      },
    })

    expect(start.ok).toBe(true)
    expect(detectMsys).not.toHaveBeenCalled()
    expect(resolveVisualStudioEnvironment).not.toHaveBeenCalled()
    expect(spawnPty).not.toHaveBeenCalled()
    expect(spawn).toHaveBeenCalledWith(
      'npm run dev',
      [],
      expect.objectContaining({
        cwd: '/repo',
        env: process.env,
        shell: true,
        windowsHide: true,
        stdio: 'pipe',
      })
    )
    expect(runner.getState()).toMatchObject({
      status: 'running',
      cwd: '/repo',
      command: 'npm run dev',
      envType: 'system',
    })

    child.emit('close', 0, null)
    await flush()

    expect(runner.getState()).toMatchObject({
      status: 'exited',
      exitCode: 0,
    })
  })

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

    ptyChild.stdout.write('\x1b[29;120Hdemo tick\n')
    await flush()

    expect(runner.getState().log).toContain('demo tick')
    expect(runner.getState().log).not.toContain('[29;120H')
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

  it('captures Windows debug output while the runtime process is active', async () => {
    const child = new FakeChild()
    const stopDebugOutputCapture = vi.fn()
    let emitDebugOutput: ((chunk: string) => void) | null = null
    const startDebugOutputCapture = vi.fn((options: { onData: (chunk: string) => void }) => {
      emitDebugOutput = options.onData
      return { stop: stopDebugOutputCapture }
    })
    const resolveVisualStudioEnvironment = vi.fn().mockResolvedValue({
      ok: true,
      displayName: 'Visual Studio 2022',
      installationPath: 'C:\\VS',
      devCmdPath: 'C:\\VS\\Common7\\Tools\\VsDevCmd.bat',
      env: { Path: 'C:\\VS\\bin' },
    })
    const runner = createRuntimeRunner({
      platform: 'win32',
      spawn: vi.fn(() => child),
      startDebugOutputCapture,
      detectMsys: vi.fn(),
      resolveVisualStudioEnvironment,
      now: () => '2026-06-12T10:00:00.000Z',
    })

    await runner.start({
      projectId: 'p-vs-debug',
      projectName: 'Demo',
      targetRepo: 'E:\\repo',
      config: {
        enabled: true,
        cwd: 'app',
        command: 'demo.exe',
        envType: 'visual-studio',
        visualStudioInstanceId: 'vs-1',
        outputEncoding: 'gbk',
      },
    })

    expect(startDebugOutputCapture).toHaveBeenCalledWith(
      expect.objectContaining({
        rootPid: 4321,
        env: { Path: 'C:\\VS\\bin' },
      })
    )

    const debugOutput = emitDebugOutput as unknown as ((chunk: string) => void) | null
    if (!debugOutput) throw new Error('debug output emitter was not registered')
    debugOutput('[debug:4322] [DLManager.cpp:3132] createDLTask\n')
    await flush()

    expect(runner.getState().log).toContain('[DLManager.cpp:3132] createDLTask')

    child.emit('close', 0, null)
    await flush()

    expect(stopDebugOutputCapture).toHaveBeenCalled()
  })

  it('adds a diagnostic when a running process produces no captured output', async () => {
    vi.useFakeTimers()
    try {
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
        noOutputNoticeMs: 10,
      })

      await runner.start({
        projectId: 'p-silent',
        projectName: 'Demo',
        targetRepo: 'E:\\repo',
        config: msysConfig,
      })

      expect(runner.getState().log).not.toContain('no stdout/stderr/debug output captured')
      vi.advanceTimersByTime(10)

      expect(runner.getState().log).toContain('no stdout/stderr/debug output captured')

      child.emit('close', 0, null)
    } finally {
      vi.useRealTimers()
    }
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
