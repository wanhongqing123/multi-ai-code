import { describe, expect, it } from 'vitest'
import {
  formatAnnotationsForSession,
  type SessionAnnotation
} from '../../../electron/orchestrator/session-messages.js'

describe('formatAnnotationsForSession', () => {
  const ann1: SessionAnnotation = {
    file: 'src/auth.ts',
    lineRange: '10-12',
    snippet: 'const token = req.headers.auth',
    comment: '改为读取 Authorization Bearer'
  }

  it('produces a markdown block starting with the batch header', () => {
    const out = formatAnnotationsForSession({ annotations: [ann1], generalComment: '' })
    expect(out.startsWith('# 用户批注')).toBe(true)
  })

  it('references each annotation with explicit file, line, snippet, and comment fields', () => {
    const out = formatAnnotationsForSession({ annotations: [ann1], generalComment: '' })
    expect(out).toContain('文件: src/auth.ts')
    expect(out).toContain('行号: 10-12')
    expect(out).toContain('代码片段:')
    expect(out).toContain('const token = req.headers.auth')
    expect(out).toContain('批注:')
    expect(out).toContain('改为读取 Authorization Bearer')
  })

  it('appends the general comment section when provided', () => {
    const out = formatAnnotationsForSession({
      annotations: [ann1],
      generalComment: '整体结构 OK，改前加一层抽象。'
    })
    expect(out).toContain('## 整体意见')
    expect(out).toContain('整体结构 OK')
  })

  it('omits the general comment section when empty', () => {
    const out = formatAnnotationsForSession({ annotations: [ann1], generalComment: '   ' })
    expect(out).not.toContain('## 整体意见')
  })

  // 普通任务删除后已经没有「方案文档」这回事：批注只针对代码改动。留一句指向
  // 不存在文件的话术，会把模型引去找一个根本没有的 md。
  it('never points the session at a plan document', () => {
    const out = formatAnnotationsForSession({ annotations: [ann1], generalComment: '' })

    expect(out).not.toContain('方案文档')
    expect(out).not.toContain('.multi-ai-code')
    expect(out).not.toContain('/ 方案')
    expect(out).toContain('直接修改代码')
    expect(out).toContain('请按照以上批注调整代码，完成后在终端里简述改了什么。')
  })

  it('handles multiple annotations in order', () => {
    const ann2: SessionAnnotation = {
      file: 'src/app.tsx',
      lineRange: '100',
      snippet: '<Login />',
      comment: '移动到 <Router> 外层'
    }
    const out = formatAnnotationsForSession({
      annotations: [ann1, ann2],
      generalComment: ''
    })
    const firstIdx = out.indexOf('文件: src/auth.ts')
    const secondIdx = out.indexOf('文件: src/app.tsx')
    expect(firstIdx).toBeGreaterThan(-1)
    expect(secondIdx).toBeGreaterThan(firstIdx)
  })

  it('handles empty annotations array without crashing', () => {
    const out = formatAnnotationsForSession({
      annotations: [],
      generalComment: '整体结构 OK'
    })
    expect(out).toContain('# 用户批注')
    expect(out).toContain('## 逐行批注')
    expect(out).toContain('## 整体意见')
    expect(out).toContain('整体结构 OK')
  })
})
