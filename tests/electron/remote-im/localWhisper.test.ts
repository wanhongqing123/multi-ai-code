import { describe, expect, it, vi } from 'vitest'
import type { RemoteImIncomingAudioMessage } from '../../../electron/remote-im/types.js'
import {
  buildLocalWhisperCommand,
  readLocalWhisperSettings,
  transcribeRemoteImAudioWithLocalWhisper
} from '../../../electron/remote-im/localWhisper.js'

const audioMessage: RemoteImIncomingAudioMessage = {
  projectId: 'project-1',
  fromUserId: 'phone_admin',
  audioUrl: 'https://cos.example.test/voice.amr',
  durationSeconds: 4
}

describe('local Whisper ASR', () => {
  it('uses built-in defaults without environment variables', () => {
    expect(readLocalWhisperSettings()).toEqual({
      command: '',
      modelPath: '',
      language: 'zh',
      timeoutMs: 120_000,
      transcoder: { command: '', kind: 'ffmpeg' }
    })
  })

  it('builds whisper.cpp command arguments with a bundled model', () => {
    expect(
      buildLocalWhisperCommand({
        settings: {
          command: '/app/asr/darwin-arm64/bin/whisper-cli',
          modelPath: '/app/asr/models/ggml-base.bin',
          language: 'zh',
          timeoutMs: 120_000,
          transcoder: { command: '/usr/bin/afconvert', kind: 'afconvert' }
        },
        audioPath: '/tmp/voice.wav',
        outputBasePath: '/tmp/voice-transcript'
      })
    ).toEqual({
      mode: 'execFile',
      command: '/app/asr/darwin-arm64/bin/whisper-cli',
      args: [
        '-m',
        '/app/asr/models/ggml-base.bin',
        '-f',
        '/tmp/voice.wav',
        '-l',
        'zh',
        '-otxt',
        '-of',
        '/tmp/voice-transcript'
      ]
    })
  })

  it('uses bundled Windows whisper-cli and ffmpeg without local configuration', async () => {
    const m4aMessage: RemoteImIncomingAudioMessage = {
      ...audioMessage,
      audioUrl: 'https://cos.example.test/voice.m4a'
    }
    const runCommand = vi.fn(async () => ({ stdout: '语音测试\n', stderr: '', exitCode: 0 }))

    const result = await transcribeRemoteImAudioWithLocalWhisper(m4aMessage, {
      platform: 'win32',
      arch: 'x64',
      asrSearchRoots: ['C:\\Program Files\\Multi-AI Code\\resources\\asr'],
      fileExists: vi.fn(async (path: string) =>
        [
          'C:\\Program Files\\Multi-AI Code\\resources\\asr/models/ggml-base.bin',
          'C:\\Program Files\\Multi-AI Code\\resources\\asr/win32-x64/bin/whisper-cli.exe',
          'C:\\Program Files\\Multi-AI Code\\resources\\asr/win32-x64/bin/ffmpeg.exe'
        ].includes(path)
      ),
      fetchArrayBuffer: vi.fn(async () => new Uint8Array([1, 2, 3]).buffer),
      runCommand,
      makeTempDir: vi.fn(async () => 'C:\\Temp\\voice-asr'),
      writeFile: vi.fn(async () => undefined),
      readTextFileIfExists: vi.fn(async () => null),
      cleanupDir: vi.fn(async () => undefined)
    })

    expect(result).toEqual({ ok: true, text: '语音测试' })
    expect(runCommand).toHaveBeenNthCalledWith(
      1,
      {
        mode: 'execFile',
        command: 'C:\\Program Files\\Multi-AI Code\\resources\\asr/win32-x64/bin/ffmpeg.exe',
        args: [
          '-y',
          '-i',
          'C:\\Temp\\voice-asr/input.m4a',
          '-ar',
          '16000',
          '-ac',
          '1',
          'C:\\Temp\\voice-asr/input.wav'
        ]
      },
      120_000
    )
    expect(runCommand).toHaveBeenNthCalledWith(
      2,
      {
        mode: 'execFile',
        command: 'C:\\Program Files\\Multi-AI Code\\resources\\asr/win32-x64/bin/whisper-cli.exe',
        args: [
          '-m',
          'C:\\Program Files\\Multi-AI Code\\resources\\asr/models/ggml-base.bin',
          '-f',
          'C:\\Temp\\voice-asr/input.wav',
          '-l',
          'zh',
          '-otxt',
          '-of',
          'C:\\Temp\\voice-asr/transcript'
        ]
      },
      120_000
    )
  })

  it('uses macOS afconvert when bundled ffmpeg is absent', async () => {
    const m4aMessage: RemoteImIncomingAudioMessage = {
      ...audioMessage,
      audioUrl: 'https://cos.example.test/voice.m4a'
    }
    const runCommand = vi.fn(async () => ({ stdout: '继续跑一下测试\n', stderr: '', exitCode: 0 }))

    const result = await transcribeRemoteImAudioWithLocalWhisper(m4aMessage, {
      platform: 'darwin',
      arch: 'arm64',
      asrSearchRoots: ['/Applications/Multi-AI Code.app/Contents/Resources/asr'],
      fileExists: vi.fn(async (path: string) =>
        [
          '/Applications/Multi-AI Code.app/Contents/Resources/asr/models/ggml-base.bin',
          '/Applications/Multi-AI Code.app/Contents/Resources/asr/darwin-arm64/bin/whisper-cli',
          '/usr/bin/afconvert'
        ].includes(path)
      ),
      fetchArrayBuffer: vi.fn(async () => new Uint8Array([1, 2, 3]).buffer),
      runCommand,
      makeTempDir: vi.fn(async () => '/tmp/voice-asr'),
      writeFile: vi.fn(async () => undefined),
      readTextFileIfExists: vi.fn(async () => null),
      cleanupDir: vi.fn(async () => undefined)
    })

    expect(result).toEqual({ ok: true, text: '继续跑一下测试' })
    expect(runCommand).toHaveBeenNthCalledWith(
      1,
      {
        mode: 'execFile',
        command: '/usr/bin/afconvert',
        args: [
          '/tmp/voice-asr/input.m4a',
          '/tmp/voice-asr/input.wav',
          '-f',
          'WAVE',
          '-d',
          'LEI16@16000',
          '-c',
          '1'
        ]
      },
      120_000
    )
    expect(runCommand).toHaveBeenNthCalledWith(
      2,
      expect.objectContaining({
        mode: 'execFile',
        command: '/Applications/Multi-AI Code.app/Contents/Resources/asr/darwin-arm64/bin/whisper-cli',
        args: expect.arrayContaining([
          '/Applications/Multi-AI Code.app/Contents/Resources/asr/models/ggml-base.bin'
        ])
      }),
      120_000
    )
  })

  it('finds bundled ASR resources from the Electron resources path', async () => {
    const runCommand = vi.fn(async () => ({
      stdout: '从 resources 路径找模型\n',
      stderr: '',
      exitCode: 0
    }))

    const result = await transcribeRemoteImAudioWithLocalWhisper(
      { ...audioMessage, audioUrl: 'https://cos.example.test/voice.mp3' },
      {
        platform: 'darwin',
        arch: 'arm64',
        resourcesPath: '/Applications/Multi-AI Code.app/Contents/Resources',
        fileExists: vi.fn(async (path: string) => {
          return path === '/Applications/Multi-AI Code.app/Contents/Resources/asr/models/ggml-base.bin' ||
            path === '/Applications/Multi-AI Code.app/Contents/Resources/asr/darwin-arm64/bin/whisper-cli' ||
            path === '/usr/bin/afconvert'
        }),
        fetchArrayBuffer: vi.fn(async () => new Uint8Array([1, 2, 3]).buffer),
        runCommand,
        makeTempDir: vi.fn(async () => '/tmp/voice-asr'),
        writeFile: vi.fn(async () => undefined),
        readTextFileIfExists: vi.fn(async () => null),
        cleanupDir: vi.fn(async () => undefined)
      }
    )

    expect(result).toEqual({ ok: true, text: '从 resources 路径找模型' })
    expect(runCommand).toHaveBeenCalledWith(
      expect.objectContaining({
        command: '/Applications/Multi-AI Code.app/Contents/Resources/asr/darwin-arm64/bin/whisper-cli',
        args: expect.arrayContaining([
          '/Applications/Multi-AI Code.app/Contents/Resources/asr/models/ggml-base.bin'
        ])
      }),
      120_000
    )
  })

  it('reports a reinstallable app error when bundled ASR runtime is missing', async () => {
    const result = await transcribeRemoteImAudioWithLocalWhisper(audioMessage, {
      platform: 'win32',
      arch: 'x64',
      asrSearchRoots: ['C:\\BrokenApp\\resources\\asr'],
      fileExists: vi.fn(async () => false),
      fetchArrayBuffer: vi.fn(),
      runCommand: vi.fn(),
      makeTempDir: vi.fn(),
      writeFile: vi.fn(),
      readTextFileIfExists: vi.fn(),
      cleanupDir: vi.fn()
    })

    if (result.ok) {
      throw new Error('Expected bundled ASR lookup to fail')
    }
    expect(result.error).toContain('本地语音转文字组件缺失')
    expect(result.error).not.toContain('MULTI_AI_CODE_WHISPER')
  })
})
