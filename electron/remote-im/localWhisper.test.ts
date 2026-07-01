import { describe, expect, it, vi } from 'vitest'
import type { RemoteImIncomingAudioMessage } from './types.js'
import {
  buildLocalWhisperCommand,
  readLocalWhisperSettings,
  transcribeRemoteImAudioWithLocalWhisper
} from './localWhisper.js'

const audioMessage: RemoteImIncomingAudioMessage = {
  projectId: 'project-1',
  fromUserId: 'phone_admin',
  audioUrl: 'https://cos.example.test/voice.amr',
  durationSeconds: 4
}

describe('local Whisper ASR', () => {
  it('reads local Whisper configuration from environment variables', () => {
    expect(
      readLocalWhisperSettings({
        MULTI_AI_CODE_WHISPER_CMD: '/opt/bin/whisper-cli',
        MULTI_AI_CODE_WHISPER_MODEL: '/models/ggml-small.bin',
        MULTI_AI_CODE_WHISPER_LANGUAGE: 'zh'
      })
    ).toEqual({
      command: '/opt/bin/whisper-cli',
        commandTemplate: '',
        modelPath: '/models/ggml-small.bin',
        language: 'zh',
        timeoutMs: 120_000,
        ffmpegCommand: ''
    })
  })

  it('builds whisper.cpp command arguments with an explicit model', () => {
    expect(
      buildLocalWhisperCommand({
        settings: {
          command: '/opt/bin/whisper-cli',
          commandTemplate: '',
          modelPath: '/models/ggml-small.bin',
          language: 'zh',
          timeoutMs: 120_000,
          ffmpegCommand: ''
        },
        audioPath: '/tmp/voice.amr',
        outputBasePath: '/tmp/voice-transcript'
      })
    ).toEqual({
      mode: 'execFile',
      command: '/opt/bin/whisper-cli',
      args: [
        '-m',
        '/models/ggml-small.bin',
        '-f',
        '/tmp/voice.amr',
        '-l',
        'zh',
        '-otxt',
        '-of',
        '/tmp/voice-transcript'
      ]
    })
  })

  it('uses command templates for custom local ASR wrappers', () => {
    expect(
      buildLocalWhisperCommand({
        settings: {
          command: '',
          commandTemplate: 'my-asr --input {input} --output {output} --lang {language}',
          modelPath: '',
          language: 'zh',
          timeoutMs: 120_000,
          ffmpegCommand: ''
        },
        audioPath: '/tmp/voice.amr',
        outputBasePath: '/tmp/voice-transcript'
      })
    ).toEqual({
      mode: 'shell',
      command: "my-asr --input '/tmp/voice.amr' --output '/tmp/voice-transcript.txt' --lang 'zh'"
    })
  })

  it('downloads the audio, runs local Whisper, and returns stdout transcript text', async () => {
    const runCommand = vi.fn(async () => ({
      stdout: '检查一下构建失败原因\n',
      stderr: '',
      exitCode: 0
    }))
    const result = await transcribeRemoteImAudioWithLocalWhisper(audioMessage, {
      env: {
        MULTI_AI_CODE_WHISPER_COMMAND:
          'mock-whisper --input {input} --output {output} --lang {language}'
      },
      fetchArrayBuffer: vi.fn(async () => new Uint8Array([1, 2, 3]).buffer),
      runCommand,
      makeTempDir: vi.fn(async () => '/tmp/voice-asr'),
      writeFile: vi.fn(async () => undefined),
      readTextFileIfExists: vi.fn(async () => null),
      cleanupDir: vi.fn(async () => undefined)
    })

    expect(result).toEqual({ ok: true, text: '检查一下构建失败原因' })
    expect(runCommand).toHaveBeenCalledWith(
      {
        mode: 'shell',
        command:
          "mock-whisper --input '/tmp/voice-asr/input.amr' --output '/tmp/voice-asr/transcript.txt' --lang 'zh'"
      },
      120_000
    )
  })

  it('uses a repository model and Homebrew whisper-cli without environment variables', async () => {
    const mp3Message: RemoteImIncomingAudioMessage = {
      ...audioMessage,
      audioUrl: 'https://cos.example.test/voice.mp3'
    }
    const runCommand = vi.fn(async () => ({
      stdout: '继续跑一下测试\n',
      stderr: '',
      exitCode: 0
    }))

    const result = await transcribeRemoteImAudioWithLocalWhisper(mp3Message, {
      env: {},
      cwd: '/repo',
      fileExists: vi.fn(async (path: string) => {
        return path === '/repo/models/whisper/ggml-base.bin' ||
          path === '/opt/homebrew/bin/whisper-cli'
      }),
      fetchArrayBuffer: vi.fn(async () => new Uint8Array([1, 2, 3]).buffer),
      runCommand,
      makeTempDir: vi.fn(async () => '/tmp/voice-asr'),
      writeFile: vi.fn(async () => undefined),
      readTextFileIfExists: vi.fn(async () => null),
      cleanupDir: vi.fn(async () => undefined)
    })

    expect(result).toEqual({ ok: true, text: '继续跑一下测试' })
    expect(runCommand).toHaveBeenCalledWith(
      {
        mode: 'execFile',
        command: '/opt/homebrew/bin/whisper-cli',
        args: [
          '-m',
          '/repo/models/whisper/ggml-base.bin',
          '-f',
          '/tmp/voice-asr/input.mp3',
          '-l',
          'zh',
          '-otxt',
          '-of',
          '/tmp/voice-asr/transcript'
        ]
      },
      120_000
    )
  })

  it('finds the repository model from the bundled main process path when cwd is another project', async () => {
    const runCommand = vi.fn(async () => ({
      stdout: '从运行产物路径找模型\n',
      stderr: '',
      exitCode: 0
    }))

    const result = await transcribeRemoteImAudioWithLocalWhisper(
      { ...audioMessage, audioUrl: 'https://cos.example.test/voice.mp3' },
      {
        env: {},
        cwd: '/Users/hongqingwan/Apollo/u3player',
        moduleUrl: 'file:///Users/hongqingwan/OpenSource/multi-ai-code/out/main/main.js',
        fileExists: vi.fn(async (path: string) => {
          return path === '/Users/hongqingwan/OpenSource/multi-ai-code/models/whisper/ggml-base.bin' ||
            path === '/opt/homebrew/bin/whisper-cli'
        }),
        fetchArrayBuffer: vi.fn(async () => new Uint8Array([1, 2, 3]).buffer),
        runCommand,
        makeTempDir: vi.fn(async () => '/tmp/voice-asr'),
        writeFile: vi.fn(async () => undefined),
        readTextFileIfExists: vi.fn(async () => null),
        cleanupDir: vi.fn(async () => undefined)
      }
    )

    expect(result).toEqual({ ok: true, text: '从运行产物路径找模型' })
    expect(runCommand).toHaveBeenCalledWith(
      expect.objectContaining({
        mode: 'execFile',
        command: '/opt/homebrew/bin/whisper-cli',
        args: expect.arrayContaining([
          '/Users/hongqingwan/OpenSource/multi-ai-code/models/whisper/ggml-base.bin'
        ])
      }),
      120_000
    )
  })

  it('transcodes m4a audio to wav before running whisper.cpp', async () => {
    const m4aMessage: RemoteImIncomingAudioMessage = {
      ...audioMessage,
      audioUrl: 'https://cos.example.test/voice.m4a'
    }
    const runCommand = vi.fn(async () => ({ stdout: '语音测试\n', stderr: '', exitCode: 0 }))

    const result = await transcribeRemoteImAudioWithLocalWhisper(m4aMessage, {
      env: {
        MULTI_AI_CODE_WHISPER_CMD: '/opt/homebrew/bin/whisper-cli',
        MULTI_AI_CODE_WHISPER_MODEL: '/repo/models/whisper/ggml-base.bin'
      },
      fileExists: vi.fn(async (path: string) => path === '/opt/homebrew/bin/ffmpeg'),
      fetchArrayBuffer: vi.fn(async () => new Uint8Array([1, 2, 3]).buffer),
      runCommand,
      makeTempDir: vi.fn(async () => '/tmp/voice-asr'),
      writeFile: vi.fn(async () => undefined),
      readTextFileIfExists: vi.fn(async () => null),
      cleanupDir: vi.fn(async () => undefined)
    })

    expect(result).toEqual({ ok: true, text: '语音测试' })
    expect(runCommand).toHaveBeenNthCalledWith(
      1,
      {
        mode: 'execFile',
        command: '/opt/homebrew/bin/ffmpeg',
        args: [
          '-y',
          '-i',
          '/tmp/voice-asr/input.m4a',
          '-ar',
          '16000',
          '-ac',
          '1',
          '/tmp/voice-asr/input.wav'
        ]
      },
      120_000
    )
    expect(runCommand).toHaveBeenNthCalledWith(
      2,
      {
        mode: 'execFile',
        command: '/opt/homebrew/bin/whisper-cli',
        args: [
          '-m',
          '/repo/models/whisper/ggml-base.bin',
          '-f',
          '/tmp/voice-asr/input.wav',
          '-l',
          'zh',
          '-otxt',
          '-of',
          '/tmp/voice-asr/transcript'
        ]
      },
      120_000
    )
  })

  it('reads the default Python whisper output file when stdout has no transcript', async () => {
    const readTextFileIfExists = vi.fn(async (path: string) => {
      return path === '/tmp/voice-asr/input.txt'
        ? '把刚才的回复转发给 B\n'
        : null
    })

    const result = await transcribeRemoteImAudioWithLocalWhisper(audioMessage, {
      env: {
        MULTI_AI_CODE_WHISPER_CMD: 'whisper',
        MULTI_AI_CODE_WHISPER_LANGUAGE: 'zh'
      },
      fetchArrayBuffer: vi.fn(async () => new Uint8Array([1, 2, 3]).buffer),
      runCommand: vi.fn(async () => ({ stdout: '', stderr: '', exitCode: 0 })),
      makeTempDir: vi.fn(async () => '/tmp/voice-asr'),
      writeFile: vi.fn(async () => undefined),
      readTextFileIfExists,
      cleanupDir: vi.fn(async () => undefined)
    })

    expect(result).toEqual({ ok: true, text: '把刚才的回复转发给 B' })
    expect(readTextFileIfExists).toHaveBeenCalledWith('/tmp/voice-asr/transcript.txt')
    expect(readTextFileIfExists).toHaveBeenCalledWith('/tmp/voice-asr/input.txt')
  })

  it('reports a configuration error when no local Whisper command is configured', async () => {
    const result = await transcribeRemoteImAudioWithLocalWhisper(audioMessage, {
      env: {},
      cwd: '/repo-without-model',
      fileExists: vi.fn(async () => false),
      fetchArrayBuffer: vi.fn(),
      runCommand: vi.fn(),
      makeTempDir: vi.fn(),
      writeFile: vi.fn(),
      readTextFileIfExists: vi.fn(),
      cleanupDir: vi.fn()
    })

    if (result.ok) {
      throw new Error('Expected local Whisper configuration to fail')
    }
    expect(result.error).toContain('本地 Whisper 未配置')
  })
})
