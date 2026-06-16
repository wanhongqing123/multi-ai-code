import { promises as fs } from 'fs'
import { mkdtemp } from 'fs/promises'
import { tmpdir } from 'os'
import { join } from 'path'
import { describe, expect, it } from 'vitest'
import {
  buildRuntimeAnalysisPromptFileMessage,
  writeRuntimeAnalysisPromptFile,
} from './analysisPromptFile.js'

describe('runtime analysis prompt files', () => {
  it('writes the full analysis prompt to a local markdown file', async () => {
    const dir = await mkdtemp(join(tmpdir(), 'multi-ai-code-runtime-analysis-test-'))

    const filePath = await writeRuntimeAnalysisPromptFile('runtime prompt\nlog line', {
      dir,
      now: new Date('2026-06-16T08:30:00.000Z'),
      randomHex: 'abc123',
    })

    expect(filePath).toBe(join(dir, 'runtime-log-2026-06-16T08-30-00-000Z-abc123.md'))
    await expect(fs.readFile(filePath, 'utf8')).resolves.toBe('runtime prompt\nlog line')
  })

  it('builds a short AICLI message that points at the prompt file', () => {
    const message = buildRuntimeAnalysisPromptFileMessage('C:\\Temp\\runtime-log.md')

    expect(message).toContain('C:\\Temp\\runtime-log.md')
    expect(message.length).toBeLessThan(300)
    expect(message).toContain('Only analyze the issue')
  })
})
