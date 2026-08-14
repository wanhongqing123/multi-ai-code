import { describe, expect, it } from 'vitest'
import { redactPtyDebugSpawnArgs } from '../../../electron/cc/PtyCCProcess.js'

describe('redactPtyDebugSpawnArgs', () => {
  it('redacts the structured bridge token without changing the real argument list', () => {
    const token = 'fixture-secret-bridge-token'
    const original = [
      '--no-alt-screen',
      '--multi-ai-code-im-ipc',
      `tcp://127.0.0.1:4321?token=${token}`,
      '--verbose'
    ]

    const redacted = redactPtyDebugSpawnArgs(original)

    expect(JSON.stringify(redacted)).not.toContain(token)
    expect(redacted[2]).toContain('token=%3Credacted%3E')
    expect(original[2]).toContain(token)
  })

  it('also redacts equals-form and malformed endpoint arguments', () => {
    const token = 'another-secret-token'
    const redacted = redactPtyDebugSpawnArgs([
      `--multi-ai-code-im-ipc=tcp://127.0.0.1:4321?token=${token}`,
      '--multi-ai-code-im-ipc',
      `not-a-url?token=${token}&mode=test`
    ])

    expect(JSON.stringify(redacted)).not.toContain(token)
    expect(redacted[2]).toBe('not-a-url?token=<redacted>&mode=test')
  })
})
