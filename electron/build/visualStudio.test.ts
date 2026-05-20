import { describe, expect, it, vi } from 'vitest'
import {
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

describe('resolveVisualStudioEnvironment', () => {
  it('fails cleanly on non-Windows platforms', async () => {
    const result = await resolveVisualStudioEnvironment({
      platform: 'linux',
      baseEnv: {},
      execFile: vi.fn(),
    })

    expect(result).toEqual({
      ok: false,
      error: 'visual-studio environment is only available on Windows',
    })
  })

  it('uses vswhere + VsDevCmd.bat to materialize build env vars', async () => {
    const execFile = vi.fn<
      (
        file: string,
        args: string[],
        options?: { cwd?: string; env?: NodeJS.ProcessEnv; timeout?: number; maxBuffer?: number }
      ) => Promise<{ stdout: string; stderr: string }>
    >()

    execFile.mockImplementationOnce(async () => ({
      stdout: 'C:\\Program Files\\Microsoft Visual Studio\\2022\\BuildTools\r\n',
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
    })

    expect(result).toEqual({
      ok: true,
      installationPath: 'C:\\Program Files\\Microsoft Visual Studio\\2022\\BuildTools',
      devCmdPath:
        'C:\\Program Files\\Microsoft Visual Studio\\2022\\BuildTools\\Common7\\Tools\\VsDevCmd.bat',
      env: {
        Path: 'C:\\VS\\bin',
        FOO: 'bar',
        VSCMD_VER: '17.9.0',
      },
    })
    expect(execFile).toHaveBeenNthCalledWith(
      1,
      'C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer\\vswhere.exe',
      ['-latest', '-products', '*', '-requires', 'Microsoft.Component.MSBuild', '-property', 'installationPath'],
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
        '"C:\\Program Files\\Microsoft Visual Studio\\2022\\BuildTools\\Common7\\Tools\\VsDevCmd.bat" -no_logo && set',
      ],
      expect.objectContaining({
        env: { Path: 'C:\\Windows\\System32', FOO: 'bar' },
      })
    )
  })

  it('reports a missing Visual Studio installation', async () => {
    const execFile = vi.fn().mockResolvedValue({ stdout: '\r\n', stderr: '' })

    const result = await resolveVisualStudioEnvironment({
      platform: 'win32',
      baseEnv: {},
      execFile,
    })

    expect(result).toEqual({
      ok: false,
      error: 'visual studio installation not found',
    })
  })
})
