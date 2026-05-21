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
    const resolveVisualStudioEnvironment = vi.fn().mockResolvedValue({
      ok: true,
      installationPath: 'C:\\VS',
      devCmdPath: 'C:\\VS\\Common7\\Tools\\VsDevCmd.bat',
      env: { Path: 'C:\\VS\\bin' },
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
      resolveVisualStudioEnvironment,
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
          visualStudioInstanceId: '',
          outputEncoding: 'auto',
        },
        {
          id: 'compile',
          name: 'Compile',
          envType: 'visual-studio',
          cwd: 'build',
          command: 'cmake --build .',
          enabled: true,
          visualStudioInstanceId: 'vs-1',
          outputEncoding: 'auto',
        },
        {
          id: 'package',
          name: 'Package',
          envType: 'visual-studio',
          cwd: 'build',
          command: 'cpack',
          enabled: true,
          visualStudioInstanceId: 'vs-1',
          outputEncoding: 'auto',
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

    expect(resolveVisualStudioEnvironment).toHaveBeenCalledWith(
      expect.objectContaining({
        instanceId: 'vs-1',
        platform: 'win32',
      })
    )
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
            visualStudioInstanceId: '',
            outputEncoding: 'auto',
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
            visualStudioInstanceId: '',
            outputEncoding: 'auto',
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
            visualStudioInstanceId: '',
            outputEncoding: 'auto',
          },
          {
            id: 'compile',
            name: 'Compile',
            envType: 'visual-studio',
            cwd: 'build',
            command: 'cmake --build .',
            enabled: true,
            visualStudioInstanceId: 'vs-1',
            outputEncoding: 'auto',
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
            visualStudioInstanceId: '',
            outputEncoding: 'auto',
          },
          {
            id: 'compile',
            name: 'Compile',
            envType: 'visual-studio',
            cwd: 'build',
            command: 'cmake --build .',
            enabled: true,
            visualStudioInstanceId: 'vs-1',
            outputEncoding: 'auto',
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
            visualStudioInstanceId: '',
            outputEncoding: 'auto',
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
            visualStudioInstanceId: '',
            outputEncoding: 'auto',
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
            visualStudioInstanceId: '',
            outputEncoding: 'auto',
          },
        ],
      },
    })

    expect(runner.stop()).toEqual({ ok: true })
    expect(child.kill).toHaveBeenCalledWith('SIGTERM')
    expect(killProcessTree).not.toHaveBeenCalled()
  })

  it('decodes stdout/stderr as gbk when the step requests gbk output', async () => {
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
    })

    await runner.start({
      projectId: 'p-gbk',
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
            command: 'echo hi',
            enabled: true,
            visualStudioInstanceId: '',
            outputEncoding: 'gbk',
          },
        ],
      },
    })

    child.stdout.write(Buffer.from([0xd6, 0xd0, 0xce, 0xc4, 0x0a]))
    child.stderr.write(Buffer.from([0xb4, 0xed, 0xce, 0xf3, 0x0a]))
    child.emit('close', 0, null)
    await flush()

    expect(runner.getState().log).toContain('中文')
    expect(runner.getState().log).toContain('错误')
  })

  it('uses gbk decoding for visual-studio steps when outputEncoding is auto', async () => {
    const child = new FakeChild()
    const runner = createBuildRunner({
      platform: 'win32',
      spawn: vi.fn(() => child),
      detectMsys: vi.fn(),
      resolveVisualStudioEnvironment: vi.fn().mockResolvedValue({
        ok: true,
        displayName: 'Visual Studio 2022',
        installationPath: 'C:\\VS',
        devCmdPath: 'C:\\VS\\Common7\\Tools\\VsDevCmd.bat',
        env: { Path: 'C:\\VS\\bin' },
      }),
      now: () => '2026-05-20T10:00:00.000Z',
    })

    await runner.start({
      projectId: 'p-auto-vs',
      projectName: 'Demo',
      targetRepo: 'E:\\repo',
      config: {
        enabled: true,
        steps: [
          {
            id: 'compile',
            name: 'Compile',
            envType: 'visual-studio',
            cwd: 'build',
            command: 'cmake --build .',
            enabled: true,
            visualStudioInstanceId: 'vs-1',
            outputEncoding: 'auto',
          },
        ],
      },
    })

    child.stdout.write(Buffer.from([0xd6, 0xd0, 0xce, 0xc4, 0x0a]))
    child.emit('close', 0, null)
    await flush()

    expect(runner.getState().log).toContain('中文')
  })

  it('uses utf8 decoding for msys steps when outputEncoding is auto', async () => {
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
    })

    await runner.start({
      projectId: 'p-auto-msys',
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
            command: 'echo hi',
            enabled: true,
            visualStudioInstanceId: '',
            outputEncoding: 'auto',
          },
        ],
      },
    })

    child.stdout.write(Buffer.from('héllo\n', 'utf8'))
    child.emit('close', 0, null)
    await flush()

    expect(runner.getState().log).toContain('héllo')
  })

  it('fails with a missing visual studio instance error when the selected instance cannot be resolved', async () => {
    const resolveVisualStudioEnvironment = vi.fn().mockResolvedValue({
      ok: false,
      error: 'visual studio instance not found: vs-missing',
    })
    const runner = createBuildRunner({
      platform: 'win32',
      spawn: vi.fn(),
      detectMsys: vi.fn(),
      resolveVisualStudioEnvironment,
      now: () => '2026-05-20T10:00:00.000Z',
    })

    await runner.start({
      projectId: 'p-vs-missing',
      projectName: 'Demo',
      targetRepo: 'E:\\repo',
      config: {
        enabled: true,
        steps: [
          {
            id: 'compile',
            name: 'Compile',
            envType: 'visual-studio',
            cwd: 'build',
            command: 'cmake --build .',
            enabled: true,
            visualStudioInstanceId: 'vs-missing',
            outputEncoding: 'auto',
          },
        ],
      },
    })

    await flush()

    expect(resolveVisualStudioEnvironment).toHaveBeenCalledWith({
      instanceId: 'vs-missing',
      platform: 'win32',
      baseEnv: process.env,
    })
    expect(runner.getState()).toMatchObject({
      status: 'failed',
      lastFailure: {
        stepId: 'compile',
        reason: 'visual studio instance not found: vs-missing',
        visualStudioInstanceId: 'vs-missing',
      },
    })
  })

  it('threads the resolved visual studio display name into failure context', async () => {
    const child = new FakeChild()
    const runner = createBuildRunner({
      platform: 'win32',
      spawn: vi.fn(() => child),
      detectMsys: vi.fn(),
      resolveVisualStudioEnvironment: vi.fn().mockResolvedValue({
        ok: true,
        displayName: 'Visual Studio 2022 Enterprise',
        installationPath: 'C:\\VS',
        devCmdPath: 'C:\\VS\\Common7\\Tools\\VsDevCmd.bat',
        env: { Path: 'C:\\VS\\bin' },
      }),
      now: () => '2026-05-20T10:00:00.000Z',
    })

    await runner.start({
      projectId: 'p-vs-display',
      projectName: 'Demo',
      targetRepo: 'E:\\repo',
      config: {
        enabled: true,
        steps: [
          {
            id: 'compile',
            name: 'Compile',
            envType: 'visual-studio',
            cwd: 'build',
            command: 'cmake --build .',
            enabled: true,
            visualStudioInstanceId: 'vs-1',
            outputEncoding: 'auto',
          },
        ],
      },
    })

    child.stderr.write('fatal error\n')
    child.emit('close', 2, null)
    await flush()

    expect(runner.getState()).toMatchObject({
      status: 'failed',
      lastFailure: {
        stepId: 'compile',
        visualStudioInstanceId: 'vs-1',
        visualStudioDisplayName: 'Visual Studio 2022 Enterprise',
        outputEncoding: 'auto',
      },
    })
  })

  it('uses utf8 decoding when the step explicitly requests utf8 output', async () => {
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
    })

    await runner.start({
      projectId: 'p-utf8',
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
            command: 'echo hi',
            enabled: true,
            visualStudioInstanceId: '',
            outputEncoding: 'utf8',
          },
        ],
      },
    })

    child.stdout.write(Buffer.from([0xe4, 0xbd, 0xa0, 0xe5, 0xa5, 0xbd, 0x0a]))
    child.emit('close', 0, null)
    await flush()

    expect(runner.getState().log).toContain('\u4f60\u597d')
  })

  it('passes string chunks through without decoding', async () => {
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
    })

    await runner.start({
      projectId: 'p-string',
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
            command: 'echo hi',
            enabled: true,
            visualStudioInstanceId: '',
            outputEncoding: 'gbk',
          },
        ],
      },
    })

    child.stdout.write('stdout text\n')
    child.stderr.write('stderr text\n')
    child.emit('close', 0, null)
    await flush()

    expect(runner.getState().log).toContain('stdout text')
    expect(runner.getState().log).toContain('stderr text')
  })

  it('preserves utf8 multibyte characters split across chunks', async () => {
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
    })

    await runner.start({
      projectId: 'p-utf8-split',
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
            command: 'echo hi',
            enabled: true,
            visualStudioInstanceId: '',
            outputEncoding: 'utf8',
          },
        ],
      },
    })

    child.stdout.write(Buffer.from([0xe4, 0xbd]))
    child.stdout.write(Buffer.from([0xa0, 0xe5, 0xa5, 0xbd, 0x0a]))
    child.emit('close', 0, null)
    await flush()

    expect(runner.getState().log).toContain('\u4f60\u597d')
  })

  it('preserves gbk multibyte characters split across chunks for explicit gbk output', async () => {
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
    })

    await runner.start({
      projectId: 'p-gbk-split',
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
            command: 'echo hi',
            enabled: true,
            visualStudioInstanceId: '',
            outputEncoding: 'gbk',
          },
        ],
      },
    })

    child.stdout.write(Buffer.from([0xd6]))
    child.stdout.write(Buffer.from([0xd0, 0xce, 0xc4, 0x0a]))
    child.emit('close', 0, null)
    await flush()

    expect(runner.getState().log).toContain('\u4e2d\u6587')
  })

  it('keeps visual studio fields null in failure context for non-VS steps', async () => {
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
    })

    await runner.start({
      projectId: 'p-non-vs-failure',
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
            command: 'echo hi',
            enabled: true,
            visualStudioInstanceId: '',
            outputEncoding: 'auto',
          },
        ],
      },
    })

    child.stderr.write('failure\n')
    child.emit('close', 1, null)
    await flush()

    expect(runner.getState()).toMatchObject({
      status: 'failed',
      lastFailure: {
        stepId: 'configure',
        visualStudioInstanceId: null,
        visualStudioDisplayName: null,
      },
    })
  })
})
