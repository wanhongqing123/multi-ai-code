import { EventEmitter } from 'events'
import { PassThrough } from 'stream'
import { afterEach, describe, expect, it, vi } from 'vitest'
import { createBuildRunner, type SpawnedBuildProcess } from './runner.js'
import type { ProjectBuildConfig } from './config.js'

class FakeChild extends EventEmitter implements SpawnedBuildProcess {
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

afterEach(() => {
  vi.restoreAllMocks()
})

describe('createBuildRunner', () => {
  it('runs enabled steps sequentially and stops on the first failure', async () => {
    const children: FakeChild[] = []
    const spawn = vi.fn(() => {
      const child = new FakeChild()
      children.push(child)
      return child
    })
    const runner = createBuildRunner({
      platform: 'win32',
      spawn,
      detectMsys: vi.fn().mockResolvedValue({
        available: true,
        bashPath: 'C:\\msys64\\usr\\bin\\bash.exe',
        usrBinDir: 'C:\\msys64\\usr\\bin',
        variant: 'msys2',
        candidates: [],
      }),
      resolveVisualStudioEnvironment: vi.fn().mockResolvedValue({
        ok: true,
        installationPath: 'C:\\VS',
        devCmdPath: 'C:\\VS\\Common7\\Tools\\VsDevCmd.bat',
        env: { Path: 'C:\\VS\\bin' },
      }),
      now: () => '2026-05-20T10:00:00.000Z',
    })

    const config: ProjectBuildConfig = {
      enabled: true,
      steps: [
        {
          id: 'configure',
          name: 'Configure',
          envType: 'msys',
          cwd: '.',
          command: 'cmake -S . -B build',
          enabled: true,
        },
        {
          id: 'compile',
          name: 'Compile',
          envType: 'visual-studio',
          cwd: 'build',
          command: 'cmake --build .',
          enabled: true,
        },
        {
          id: 'package',
          name: 'Package',
          envType: 'visual-studio',
          cwd: 'build',
          command: 'cpack',
          enabled: true,
        },
      ],
    }

    const start = await runner.start({
      projectId: 'p1',
      projectName: 'Demo',
      targetRepo: 'E:\\repo',
      config,
    })

    expect(start.ok).toBe(true)
    expect(spawn).toHaveBeenNthCalledWith(
      1,
      'C:\\msys64\\usr\\bin\\bash.exe',
      ['-lc', "cd '/e/repo' && cmake -S . -B build"],
      expect.objectContaining({
        cwd: 'E:\\repo',
        shell: false,
        windowsHide: true,
      })
    )

    children[0].stdout.write('configure ok\n')
    children[0].emit('close', 0, null)
    await flush()

    expect(spawn).toHaveBeenNthCalledWith(
      2,
      'cmd.exe',
      ['/d', '/s', '/c', 'cmake --build .'],
      expect.objectContaining({
        cwd: 'E:\\repo\\build',
        shell: false,
        windowsHide: true,
        env: { Path: 'C:\\VS\\bin' },
      })
    )

    children[1].stderr.write('fatal error C1000\n')
    children[1].emit('close', 2, null)
    await flush()

    expect(spawn).toHaveBeenCalledTimes(2)
    expect(runner.getState()).toMatchObject({
      status: 'failed',
      projectId: 'p1',
      activeStepId: null,
      lastFailure: {
        stepId: 'compile',
        exitCode: 2,
        command: 'cmake --build .',
        targetRepo: 'E:\\repo',
      },
      steps: [
        { id: 'configure', status: 'succeeded' },
        { id: 'compile', status: 'failed' },
        { id: 'package', status: 'pending' },
      ],
    })
    expect(runner.getState().log).toContain('configure ok')
    expect(runner.getState().log).toContain('fatal error C1000')
  })

  it('truncates in-memory run log with a visible marker once the limit is exceeded', async () => {
    const child = new FakeChild()
    const runner = createBuildRunner({
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
      now: () => '2026-05-20T10:00:00.000Z',
      logLimit: 80,
    })

    await runner.start({
      projectId: 'p-log',
      projectName: 'Demo',
      targetRepo: 'E:\\repo',
      config: {
        enabled: true,
        steps: [
          {
            id: 'configure',
            name: 'Configure',
            envType: 'msys',
            cwd: '.',
            command: 'cmake -S . -B build',
            enabled: true,
          },
        ],
      },
    })

    child.stdout.write('1234567890abcdefghij1234567890abcdefghij1234567890abcdefghij1234567890\n')
    child.stderr.write('tail-marker\n')
    child.emit('close', 0, null)
    await flush()

    const log = runner.getState().log
    expect(log).toContain('[build] earlier log truncated...')
    expect(log).toContain('tail-marker')
    expect(log.length).toBeLessThanOrEqual(80)
  })

  it('kills the active child and transitions to canceled on stop', async () => {
    const child = new FakeChild()
    const spawn = vi.fn(() => child)
    const killProcessTree = vi.fn()
    const runner = createBuildRunner({
      platform: 'win32',
      spawn,
      killProcessTree,
      detectMsys: vi.fn().mockResolvedValue({
        available: true,
        bashPath: 'C:\\msys64\\usr\\bin\\bash.exe',
        usrBinDir: 'C:\\msys64\\usr\\bin',
        variant: 'msys2',
        candidates: [],
      }),
      resolveVisualStudioEnvironment: vi.fn(),
      now: () => '2026-05-20T10:00:00.000Z',
    })

    await runner.start({
      projectId: 'p2',
      projectName: 'Demo',
      targetRepo: 'E:\\repo',
      config: {
        enabled: true,
        steps: [
          {
            id: 'configure',
            name: 'Configure',
            envType: 'msys',
            cwd: '.',
            command: 'cmake -S . -B build',
            enabled: true,
          },
        ],
      },
    })

    expect(runner.stop()).toEqual({ ok: true })
    expect(killProcessTree).toHaveBeenCalledWith(4321)
    expect(child.kill).not.toHaveBeenCalled()

    child.emit('close', null, 'SIGTERM')
    await flush()

    expect(runner.getState()).toMatchObject({
      status: 'stopped',
      steps: [{ id: 'configure', status: 'skipped' }],
    })
  })

  it('marks the run stopped when stop is requested between two steps', async () => {
    const children: FakeChild[] = []
    const spawn = vi.fn(() => {
      const child = new FakeChild()
      children.push(child)
      return child
    })
    const runner = createBuildRunner({
      platform: 'win32',
      spawn,
      detectMsys: vi.fn().mockResolvedValue({
        available: true,
        bashPath: 'C:\\msys64\\usr\\bin\\bash.exe',
        usrBinDir: 'C:\\msys64\\usr\\bin',
        variant: 'msys2',
        candidates: [],
      }),
      resolveVisualStudioEnvironment: vi.fn().mockResolvedValue({
        ok: true,
        installationPath: 'C:\\VS',
        devCmdPath: 'C:\\VS\\Common7\\Tools\\VsDevCmd.bat',
        env: { Path: 'C:\\VS\\bin' },
      }),
      now: () => '2026-05-20T10:00:00.000Z',
    })

    const off = runner.onStatus((state) => {
      if (
        state.status === 'running' &&
        state.activeStepId === null &&
        state.steps[0]?.status === 'succeeded' &&
        state.steps[1]?.status === 'pending'
      ) {
        runner.stop()
      }
    })

    await runner.start({
      projectId: 'p-stop-gap',
      projectName: 'Demo',
      targetRepo: 'E:\\repo',
      config: {
        enabled: true,
        steps: [
          {
            id: 'configure',
            name: 'Configure',
            envType: 'msys',
            cwd: '.',
            command: 'cmake -S . -B build',
            enabled: true,
          },
          {
            id: 'compile',
            name: 'Compile',
            envType: 'visual-studio',
            cwd: 'build',
            command: 'cmake --build .',
            enabled: true,
          },
        ],
      },
    })

    children[0].emit('close', 0, null)
    await flush()
    off()

    expect(runner.getState()).toMatchObject({
      status: 'stopped',
      steps: [
        { id: 'configure', status: 'succeeded' },
        { id: 'compile', status: 'skipped' },
      ],
    })
    expect(spawn).toHaveBeenCalledTimes(1)
  })

  it('preserves the current step result when stop happens after close but before result handling', async () => {
    const children: FakeChild[] = []
    const spawn = vi.fn(() => {
      const child = new FakeChild()
      children.push(child)
      return child
    })
    const runner = createBuildRunner({
      platform: 'win32',
      spawn,
      detectMsys: vi.fn().mockResolvedValue({
        available: true,
        bashPath: 'C:\\msys64\\usr\\bin\\bash.exe',
        usrBinDir: 'C:\\msys64\\usr\\bin',
        variant: 'msys2',
        candidates: [],
      }),
      resolveVisualStudioEnvironment: vi.fn().mockResolvedValue({
        ok: true,
        installationPath: 'C:\\VS',
        devCmdPath: 'C:\\VS\\Common7\\Tools\\VsDevCmd.bat',
        env: { Path: 'C:\\VS\\bin' },
      }),
      now: () => '2026-05-20T10:00:00.000Z',
    })

    await runner.start({
      projectId: 'p-race',
      projectName: 'Demo',
      targetRepo: 'E:\\repo',
      config: {
        enabled: true,
        steps: [
          {
            id: 'configure',
            name: 'Configure',
            envType: 'msys',
            cwd: '.',
            command: 'cmake -S . -B build',
            enabled: true,
          },
          {
            id: 'compile',
            name: 'Compile',
            envType: 'visual-studio',
            cwd: 'build',
            command: 'cmake --build .',
            enabled: true,
          },
        ],
      },
    })

    children[0].emit('close', 0, null)
    expect(runner.stop()).toEqual({ ok: true })
    await flush()

    expect(runner.getState()).toMatchObject({
      status: 'stopped',
      steps: [
        { id: 'configure', status: 'succeeded' },
        { id: 'compile', status: 'skipped' },
      ],
    })
    expect(spawn).toHaveBeenCalledTimes(1)
  })

  it('fails immediately when msys is requested but unavailable', async () => {
    const runner = createBuildRunner({
      platform: 'win32',
      spawn: vi.fn(),
      detectMsys: vi.fn().mockResolvedValue({
        available: false,
        bashPath: null,
        usrBinDir: null,
        variant: null,
        candidates: [],
      }),
      resolveVisualStudioEnvironment: vi.fn(),
      now: () => '2026-05-20T10:00:00.000Z',
    })

    await runner.start({
      projectId: 'p3',
      projectName: 'Demo',
      targetRepo: 'E:\\repo',
      config: {
        enabled: true,
        steps: [
          {
            id: 'configure',
            name: 'Configure',
            envType: 'msys',
            cwd: '.',
            command: 'cmake -S . -B build',
            enabled: true,
          },
        ],
      },
    })

    await flush()

    expect(runner.getState()).toMatchObject({
      status: 'failed',
      lastFailure: {
        stepId: 'configure',
        reason: 'msys environment is unavailable',
      },
    })
  })

  it('rejects cwd values that escape targetRepo', async () => {
    const runner = createBuildRunner({
      platform: 'win32',
      spawn: vi.fn(),
      detectMsys: vi.fn(),
      resolveVisualStudioEnvironment: vi.fn(),
      now: () => '2026-05-20T10:00:00.000Z',
    })

    await runner.start({
      projectId: 'p-escape',
      projectName: 'Demo',
      targetRepo: 'E:\\repo',
      config: {
        enabled: true,
        steps: [
          {
            id: 'escape',
            name: 'Escape',
            envType: 'msys',
            cwd: '..\\outside',
            command: 'echo nope',
            enabled: true,
          },
        ],
      },
    })

    await flush()

    expect(runner.getState()).toMatchObject({
      status: 'failed',
      lastFailure: {
        stepId: 'escape',
        reason: 'cwd must stay within target_repo',
      },
    })
  })

  it('uses direct signal kill on non-Windows platforms', async () => {
    const child = new FakeChild()
    const killProcessTree = vi.fn()
    const runner = createBuildRunner({
      platform: 'linux',
      spawn: vi.fn(() => child),
      killProcessTree,
      detectMsys: vi.fn().mockResolvedValue({
        available: true,
        bashPath: '/usr/bin/bash',
        usrBinDir: '/usr/bin',
        variant: 'path',
        candidates: [],
      }),
      resolveVisualStudioEnvironment: vi.fn(),
      now: () => '2026-05-20T10:00:00.000Z',
    })

    await runner.start({
      projectId: 'p-linux-stop',
      projectName: 'Demo',
      targetRepo: '/repo',
      config: {
        enabled: true,
        steps: [
          {
            id: 'configure',
            name: 'Configure',
            envType: 'msys',
            cwd: '.',
            command: 'cmake -S . -B build',
            enabled: true,
          },
        ],
      },
    })

    expect(runner.stop()).toEqual({ ok: true })
    expect(child.kill).toHaveBeenCalledWith('SIGTERM')
    expect(killProcessTree).not.toHaveBeenCalled()
  })
})
