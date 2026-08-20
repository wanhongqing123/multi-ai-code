import { mkdtempSync, readFileSync, rmSync, statSync } from 'fs'
import { tmpdir } from 'os'
import { join } from 'path'
import { afterEach, describe, expect, it } from 'vitest'
import {
  readOpenCodeApiKey,
  readOpenCodeCredentialEnv,
  writeOpenCodeApiKey
} from '../../../electron/aicli/opencodeCredentials.js'

const roots: string[] = []

afterEach(() => {
  for (const root of roots.splice(0)) rmSync(root, { recursive: true, force: true })
})

function runtimeRoot(): string {
  const root = mkdtempSync(join(tmpdir(), 'opencode-credentials-'))
  roots.push(root)
  return root
}

describe('OpenCode account-local credentials', () => {
  it('returns no credential before the user configures one', () => {
    const root = runtimeRoot()
    expect(readOpenCodeApiKey(root)).toBe('')
    expect(readOpenCodeCredentialEnv(root)).toEqual({})
  })

  it('stores a trimmed API Key in a private account-local file', () => {
    const root = runtimeRoot()
    writeOpenCodeApiKey(root, '  test-zhipu-key  ')

    expect(readOpenCodeApiKey(root)).toBe('test-zhipu-key')
    expect(readOpenCodeCredentialEnv(root)).toEqual({ ZHIPU_API_KEY: 'test-zhipu-key' })
    expect(JSON.parse(readFileSync(join(root, 'managed-credentials.json'), 'utf8'))).toEqual({
      version: 1,
      zhipuApiKey: 'test-zhipu-key'
    })
    if (process.platform !== 'win32') {
      expect(statSync(join(root, 'managed-credentials.json')).mode & 0o777).toBe(0o600)
    }
  })

  it('deletes the local credential when the saved value is cleared', () => {
    const root = runtimeRoot()
    writeOpenCodeApiKey(root, 'test-zhipu-key')
    writeOpenCodeApiKey(root, '   ')
    expect(readOpenCodeApiKey(root)).toBe('')
  })
})
