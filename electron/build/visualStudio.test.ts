import { describe, expect, it, vi } from 'vitest'
import {
  listVisualStudioInstallations,
  parseVisualStudioEnvironmentBlock,
  resolveVisualStudioEnvironment,
} from './visualStudio.js'

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
          installationPath: 'D:\\VS\\Enterprise',
          productLineVersion: '2022',
          isPrerelease: false,
        },
      ]),
      stderr: '',
    }))
    execFile.mockImplementationOnce(async () => ({
      stdout: 'Path=C:\\VS\\bin\r\nVSCMD_VER=17.9.0\r\n',
      stderr: '',
    }))

    const result = await resolveVisualStudioEnvironment({
      platform: 'win32',
      baseEnv: { Path: 'C:\\Windows\\System32', FOO: 'bar' },
      execFile,
      instanceId: 'b2',
    })

    expect(result).toEqual({
      ok: true,
      displayName: 'Visual Studio Enterprise 2022',
      installationPath: 'D:\\VS\\Enterprise',
      devCmdPath: 'D:\\VS\\Enterprise\\Common7\\Tools\\VsDevCmd.bat',
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
    expect(execFile).toHaveBeenNthCalledWith(
      2,
      'cmd.exe',
      [
        '/d',
        '/s',
        '/c',
        '"D:\\VS\\Enterprise\\Common7\\Tools\\VsDevCmd.bat" -no_logo && set',
      ],
      expect.objectContaining({
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
})
