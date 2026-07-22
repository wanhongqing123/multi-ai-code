import { mkdtemp, readFile } from 'fs/promises'
import { tmpdir } from 'os'
import { join } from 'path'
import { describe, expect, it } from 'vitest'
import {
  appendRemoteImRuntimeLog,
  normalizeRemoteImRuntimeLogEntry
} from '../../../electron/remote-im/runtimeLog.js'

describe('remote IM runtime log', () => {
  it('normalizes diagnostic entries and redacts credential-like details', () => {
    const entry = normalizeRemoteImRuntimeLogEntry({
      projectId: 'project-1',
      sdkAppId: 1600148979,
      desktopUserId: 'test1234',
      peerUserId: 'test12345',
      messageId: 42,
      event: 'send:start',
      detail: {
        userSig: 'sig-value',
        userSigSecretKey: 'secret-value',
        nested: {
          SecretKey: 'secret-value-2',
          code: 0
        }
      },
      createdAt: 1782238800000
    })

    expect(entry).toEqual({
      projectId: 'project-1',
      sdkAppId: 1600148979,
      desktopUserId: 'test1234',
      peerUserId: 'test12345',
      messageId: 42,
      event: 'send:start',
      detail: {
        userSig: '[redacted]',
        userSigSecretKey: '[redacted]',
        nested: {
          SecretKey: '[redacted]',
          code: 0
        }
      },
      createdAt: 1782238800000
    })
  })

  it('appends diagnostic entries as JSON lines under the app root', async () => {
    const root = await mkdtemp(join(tmpdir(), 'remote-im-runtime-log-'))

    await appendRemoteImRuntimeLog(root, {
      event: 'send:resolved',
      projectId: 'project-1',
      desktopUserId: 'test1234',
      peerUserId: 'test12345',
      messageId: 42,
      detail: { code: 0 },
      createdAt: 1782238800000
    })

    const content = await readFile(join(root, 'remote-im-runtime.log'), 'utf8')
    expect(content.trim()).toBe(
      JSON.stringify({
        projectId: 'project-1',
        sdkAppId: null,
        desktopUserId: 'test1234',
        peerUserId: 'test12345',
        messageId: 42,
        event: 'send:resolved',
        detail: { code: 0 },
        createdAt: 1782238800000
      })
    )
  })
})
