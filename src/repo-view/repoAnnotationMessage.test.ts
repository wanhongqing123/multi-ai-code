import { describe, expect, it } from 'vitest'
import { buildRepoAnnotationMessage } from './repoAnnotationMessage.js'

describe('buildRepoAnnotationMessage', () => {
  it('includes per-annotation comments and the overall question', () => {
    const text = buildRepoAnnotationMessage({
      filePath: 'src/app.ts',
      question: '请直接修改这段逻辑',
      annotations: [
        {
          id: 'a1',
          filePath: 'src/app.ts',
          lineRange: '12-18',
          snippet: 'const value = compute()',
          comment: '这里要避免重复计算'
        },
        {
          id: 'a2',
          filePath: 'src/app.ts',
          lineRange: '30',
          snippet: 'return value',
          comment: '补一个空值保护'
        }
      ]
    })

    expect(text).toContain('文件：src/app.ts')
    expect(text).toContain('片段 1')
    expect(text).toContain('这里要避免重复计算')
    expect(text).toContain('补一个空值保护')
    expect(text).toContain('请直接修改这段逻辑')
  })
})
