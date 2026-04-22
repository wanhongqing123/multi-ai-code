import { describe, expect, it } from 'vitest'
import { buildRepoAnalysisPrompt } from './analysisPrompt.js'

describe('buildRepoAnalysisPrompt', () => {
  it('includes no-write constraints and memory block contract', () => {
    const text = buildRepoAnalysisPrompt({
      repoRoot: '/repo',
      filePath: 'src/app.ts',
      selection: 'function demo() {}',
      question: '这段逻辑做什么？',
      projectSummary: 'summary',
      fileNote: 'file note'
    })
    expect(text).toContain('不要修改仓库文件')
    expect(text).toContain('[[MEMORY_UPDATE]]')
    expect(text).toContain('[[END_OF_ANALYSIS]]')
    expect(text).toContain('src/app.ts')
  })
})
