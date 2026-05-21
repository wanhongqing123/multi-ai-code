import { EventEmitter } from 'events'
import { mkdirSync, writeFileSync } from 'fs'
import iconv from 'iconv-lite'
import { tmpdir } from 'os'
import { join } from 'path'
import { PassThrough } from 'stream'
import { describe, expect, it, vi } from 'vitest'
import {
  listVisualStudioInstallations,
  parseVisualStudioEnvironmentBlock,
  runWindowsCommandVerbatim,
  resolveVisualStudioEnvironment,
} from './visualStudio.js'

class FakeSpawnChild extends EventEmitter {
  stdout = new PassThrough()
  stderr = new PassThrough()

  kill(): boolean {
    return true
  }
}

describe('parseVisualStudioEnvironmentBlock', () => {
  it('parses cmd.exe set output into an env object', () => {
    expect(parseVisualStudioEnvironmentBlock('Path=C:\\VS\\bin\r\nINCLUDE=C:\\VS\\include\r\n')).toEqual(
      {
        Path: 'C:\\VS\\bin',
        INCLUDE: 'C:\\VS\\include',
      }
    )
  })
})

describe('listVisualStudioInstallations', () => {
  it('maps multiple Visual Studio installations from vswhere json and filters missing paths', async () => {
    const execFile = vi.fn<
      (
        file: string,
        args: string[],
        options?: { cwd?: string; env?: NodeJS.ProcessEnv; timeout?: number; maxBuffer?: number }
      ) => Promise<{ stdout: string; stderr: string }>
    >()

    execFile.mockResolvedValue({
      stdout: JSON.stringify([
        {
          instanceId: 'a1',
          displayName: 'Visual Studio Build Tools 2022',
          installationPath: 'C:\\VS\\BuildTools',
          productLineVersion: '2022',
          catalog: {
            productLineVersion: '2022',
          },
          isPrerelease: false,
        },
        {
          instanceId: 'skip-me',
          displayName: 'Broken Instance',
          installationPath: '',
          productLineVersion: '2019',
          isPrerelease: false,
        },
        {
          instanceId: 'b2',
          displayName: 'Visual Studio Enterprise 2022 Preview',
          installationPath: 'D:\\VS\\Enterprise',
          catalog: {
            productLineVersion: '2022',
          },
          isPrerelease: true,
        },
      ]),
      stderr: '',
    })

    const result = await listVisualStudioInstallations({
      platform: 'win32',
      baseEnv: { SystemRoot: 'C:\\Windows' },
      execFile,
    })

    expect(result).toEqual({
      ok: true,
      value: [
        {
          instanceId: 'a1',
          displayName: 'Visual Studio Build Tools 2022',
          installationPath: 'C:\\VS\\BuildTools',
          productLineVersion: '2022',
          isPrerelease: false,
        },
        {
          instanceId: 'b2',
          displayName: 'Visual Studio Enterprise 2022 Preview',
          installationPath: 'D:\\VS\\Enterprise',
          productLineVersion: '2022',
          isPrerelease: true,
        },
      ],
    })
    expect(execFile).toHaveBeenCalledWith(
      'C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer\\vswhere.exe',
      [
        '-nologo',
        '-utf8',
        '-prerelease',
        '-products',
        '*',
        '-requires',
        'Microsoft.Component.MSBuild',
        '-format',
        'json',
      ],
      expect.objectContaining({
        env: { SystemRoot: 'C:\\Windows' },
      })
    )
  })

  it('returns an error when vswhere output is valid json but not an array', async () => {
    const execFile = vi.fn().mockResolvedValue({
      stdout: JSON.stringify({ instanceId: 'a1' }),
      stderr: '',
    })

    const result = await listVisualStudioInstallations({
      platform: 'win32',
      baseEnv: {},
      execFile,
    })

    expect(result).toEqual({
      ok: false,
      error: 'vswhere output was not an array',
    })
  })
})

describe('resolveVisualStudioEnvironment', () => {
  it('runs a batch file under a spaced path with verbatim cmd arguments', async () => {
    const scriptDir = join(tmpdir(), `codex-vs-${Date.now()}`, 'Visual Studio 2022', 'Common7', 'Tools')
    mkdirSync(scriptDir, { recursive: true })
    const batPath = join(scriptDir, 'VsDevCmd.bat')
    writeFileSync(batPath, '@echo off\r\necho BAT_OK\r\n')

    const result = await runWindowsCommandVerbatim(`call "${batPath}"`, {
      env: { SystemRoot: process.env.SystemRoot ?? 'C:\\Windows' },
      timeout: 5000,
      maxBuffer: 1024 * 1024,
    })

    expect(result.stdout.toString('utf8')).toContain('BAT_OK')
  })

  it('fails cleanly on non-Windows platforms', async () => {
    const result = await resolveVisualStudioEnvironment({
      platform: 'linux',
      baseEnv: {},
      execFile: vi.fn(),
      instanceId: 'a1',
    })

    expect(result).toEqual({
      ok: false,
      error: 'visual-studio environment is only available on Windows',
    })
  })

  it('uses the selected Visual Studio instance to materialize build env vars', async () => {
    const execFile = vi.fn<
      (
        file: string,
        args: string[],
        options?: { cwd?: string; env?: NodeJS.ProcessEnv; timeout?: number; maxBuffer?: number }
      ) => Promise<{ stdout: string; stderr: string }>
    >()

    const child = new FakeSpawnChild()
    const spawn = vi.fn(() => {
      setImmediate(() => {
        child.stdout.write(Buffer.from('Path=C:\\VS\\bin\r\nVSCMD_VER=17.9.0\r\n', 'utf8'))
        child.stdout.end()
        child.stderr.end()
        child.emit('close', 0, null)
      })
      return child as never
    })

    execFile.mockImplementationOnce(async () => ({
      stdout: JSON.stringify([
        {
          instanceId: 'a1',
          displayName: 'Visual Studio Build Tools 2022',
          installationPath: 'C:\\Program Files\\Microsoft Visual Studio\\2022\\BuildTools',
          productLineVersion: '2022',
          isPrerelease: false,
        },
        {
          instanceId: 'b2',
          displayName: 'Visual Studio Enterprise 2022',
          installationPath: 'C:\\Program Files\\Microsoft Visual Studio\\2022\\Enterprise',
          productLineVersion: '2022',
          isPrerelease: false,
        },
      ]),
      stderr: '',
    }))

    const result = await resolveVisualStudioEnvironment({
      platform: 'win32',
      baseEnv: { Path: 'C:\\Windows\\System32', FOO: 'bar' },
      execFile,
      spawn,
      instanceId: 'b2',
    })

    expect(result).toEqual({
      ok: true,
      displayName: 'Visual Studio Enterprise 2022',
      installationPath: 'C:\\Program Files\\Microsoft Visual Studio\\2022\\Enterprise',
      devCmdPath:
        'C:\\Program Files\\Microsoft Visual Studio\\2022\\Enterprise\\Common7\\Tools\\VsDevCmd.bat',
      env: {
        Path: 'C:\\VS\\bin',
        FOO: 'bar',
        VSCMD_VER: '17.9.0',
      },
    })
    expect(execFile).toHaveBeenNthCalledWith(
      1,
      'C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer\\vswhere.exe',
      [
        '-nologo',
        '-utf8',
        '-prerelease',
        '-products',
        '*',
        '-requires',
        'Microsoft.Component.MSBuild',
        '-format',
        'json',
      ],
      expect.objectContaining({
        env: { Path: 'C:\\Windows\\System32', FOO: 'bar' },
      })
    )
    expect(execFile).toHaveBeenCalledTimes(1)
    expect(spawn).toHaveBeenCalledWith(
      'cmd.exe',
      [
        '/d',
        '/s',
        '/c',
        'call "C:\\Program Files\\Microsoft Visual Studio\\2022\\Enterprise\\Common7\\Tools\\VsDevCmd.bat" -no_logo && set',
      ],
      expect.objectContaining({
        windowsHide: true,
        windowsVerbatimArguments: true,
        stdio: ['ignore', 'pipe', 'pipe'],
        env: { Path: 'C:\\Windows\\System32', FOO: 'bar' },
      })
    )
  })

  it('reports a missing Visual Studio instance id', async () => {
    const execFile = vi.fn().mockResolvedValue({
      stdout: JSON.stringify([
        {
          instanceId: 'a1',
          displayName: 'Visual Studio Build Tools 2022',
          installationPath: 'C:\\VS\\BuildTools',
          productLineVersion: '2022',
          isPrerelease: false,
        },
      ]),
      stderr: '',
    })

    const result = await resolveVisualStudioEnvironment({
      platform: 'win32',
      baseEnv: {},
      execFile,
      instanceId: 'b2',
    })

    expect(result).toEqual({
      ok: false,
      error: 'visual studio instance not found: b2',
    })
  })

  it('decodes cmd.exe failures from gbk when VsDevCmd cannot be invoked', async () => {
    const execFile = vi.fn<
      (
        file: string,
        args: string[],
        options?: { cwd?: string; env?: NodeJS.ProcessEnv; timeout?: number; maxBuffer?: number }
      ) => Promise<{ stdout: string | Buffer; stderr: string | Buffer }>
    >()

    execFile.mockImplementationOnce(async () => ({
      stdout: JSON.stringify([
        {
          instanceId: 'a1',
          displayName: 'Visual Studio Community 2022',
          installationPath: 'C:\\Program Files\\Microsoft Visual Studio\\2022\\Community',
          productLineVersion: '2022',
          isPrerelease: false,
        },
      ]),
      stderr: '',
    }))
    const child = new FakeSpawnChild()
    const spawn = vi.fn(() => {
      setImmediate(() => {
        child.stderr.write(
          iconv.encode(
            `'"C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\Common7\\Tools\\VsDevCmd.bat"' 不是内部或外部命令，也不是可运行的程序或批处理文件。`,
            'gbk'
          )
        )
        child.stderr.end()
        child.stdout.end()
        child.emit('close', 1, null)
      })
      return child as never
    })

    const result = await resolveVisualStudioEnvironment({
      platform: 'win32',
      baseEnv: { Path: 'C:\\Windows\\System32' },
      execFile,
      spawn,
      instanceId: 'a1',
    })

    expect(result).toEqual({
      ok: false,
      error:
        `'"C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\Common7\\Tools\\VsDevCmd.bat"' ` +
        '不是内部或外部命令，也不是可运行的程序或批处理文件。',
    })
  })
})
