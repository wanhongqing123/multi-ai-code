import { describe, it, expect } from 'vitest'
import {
  formatInitialMessage,
  formatAnnotationsForSession,
  type SessionAnnotation
} from './session-messages.js'

describe('formatInitialMessage', () => {
  it('returns a "please continue" message when plan file content is non-null', () => {
    const out = formatInitialMessage({
      planName: 'add-auth',
      planAbsPath: '/repo/.multi-ai-code/designs/add-auth.md',
      planSource: 'internal',
      planContent: '# 方案：增加 OAuth\n\n详细步骤...'
    })
    expect(out).toContain('# 方案：增加 OAuth')
    expect(out).toContain('详细步骤')
    expect(out).toContain('此时不要修改任何代码')
  })

  it('returns a "kick off design" message when plan content is null', () => {
    const out = formatInitialMessage({
      planName: 'add-auth',
      planAbsPath: '/repo/.multi-ai-code/designs/add-auth.md',
      planSource: 'internal',
      planContent: null
    })
    expect(out).toContain('add-auth')
    expect(out).toContain('/repo/.multi-ai-code/designs/add-auth.md')
    expect(out).toContain('澄清需求')
  })

  it('treats empty planContent as missing (kick-off branch)', () => {
    const out = formatInitialMessage({
      planName: 'add-auth',
      planAbsPath: '/repo/.multi-ai-code/designs/add-auth.md',
      planSource: 'internal',
      planContent: ''
    })
    expect(out).toContain('add-auth')
    expect(out).toContain('/repo/.multi-ai-code/designs/add-auth.md')
    expect(out).toContain('澄清需求')
    expect(out).not.toContain('此时不要修改任何代码')
  })

  it('treats whitespace-only planContent as missing (kick-off branch)', () => {
    const out = formatInitialMessage({
      planName: 'add-auth',
      planAbsPath: '/repo/.multi-ai-code/designs/add-auth.md',
      planSource: 'internal',
      planContent: '   \n\n  '
    })
    expect(out).toContain('澄清需求')
    expect(out).not.toContain('此时不要修改任何代码')
  })

  it('uses path-only instructions for external plans instead of inlining content', () => {
    const out = formatInitialMessage({
      planName: 'vendor-plan',
      planAbsPath: '/external/vendor-plan.md',
      planSource: 'external',
      planContent: '# 外部方案\n\n这里不应直接发给 AI'
    })
    expect(out).toContain('/external/vendor-plan.md')
    expect(out).toContain('请先自行读取这个方案文件')
    expect(out).not.toContain('这里不应直接发给 AI')
  })
})

describe('formatAnnotationsForSession', () => {
  const ann1: SessionAnnotation = {
    file: 'src/auth.ts',
    lineRange: '10-12',
    snippet: 'const token = req.headers.auth',
    comment: '改为读取 Authorization Bearer'
  }

  it('produces a markdown block starting with the batch header', () => {
    const out = formatAnnotationsForSession({
      annotations: [ann1],
      generalComment: '',
      planAbsPath: '/repo/.multi-ai-code/designs/add-auth.md'
    })
    expect(out.startsWith('# 用户批注')).toBe(true)
  })

  it('references each annotation with file:line + snippet + comment', () => {
    const out = formatAnnotationsForSession({
      annotations: [ann1],
      generalComment: '',
      planAbsPath: '/repo/.multi-ai-code/designs/add-auth.md'
    })
    expect(out).toContain('src/auth.ts:10-12')
    expect(out).toContain('const token = req.headers.auth')
    expect(out).toContain('改为读取 Authorization Bearer')
  })

  it('appends the general comment section when provided', () => {
    const out = formatAnnotationsForSession({
      annotations: [ann1],
      generalComment: '整体结构 OK，改前加一层抽象',
      planAbsPath: '/repo/.multi-ai-code/designs/add-auth.md'
    })
    expect(out).toContain('## 整体意见')
    expect(out).toContain('整体结构 OK')
  })

  it('omits the general comment section when empty', () => {
    const out = formatAnnotationsForSession({
      annotations: [ann1],
      generalComment: '   ',
      planAbsPath: '/repo/.multi-ai-code/designs/add-auth.md'
    })
    expect(out).not.toContain('## 整体意见')
  })

  it('embeds the plan absolute path so AI can update the plan if asked', () => {
    const out = formatAnnotationsForSession({
      annotations: [ann1],
      generalComment: '',
      planAbsPath: '/repo/.multi-ai-code/designs/add-auth.md'
    })
    expect(out).toContain('/repo/.multi-ai-code/designs/add-auth.md')
  })

  it('handles multiple annotations in order', () => {
    const ann2: SessionAnnotation = {
      file: 'src/app.tsx',
      lineRange: '100',
      snippet: '<Login />',
      comment: '移到 <Router> 外'
    }
    const out = formatAnnotationsForSession({
      annotations: [ann1, ann2],
      generalComment: '',
      planAbsPath: '/repo/.multi-ai-code/designs/add-auth.md'
    })
    const firstIdx = out.indexOf('src/auth.ts:10-12')
    const secondIdx = out.indexOf('src/app.tsx:100')
    expect(firstIdx).toBeGreaterThan(-1)
    expect(secondIdx).toBeGreaterThan(firstIdx)
  })

  it('handles empty annotations array without crashing', () => {
    const out = formatAnnotationsForSession({
      annotations: [],
      generalComment: '整体结构 OK',
      planAbsPath: '/repo/.multi-ai-code/designs/add-auth.md'
    })
    expect(out).toContain('# 用户批注')
    expect(out).toContain('## 逐行批注')
    expect(out).toContain('## 整体意见')
    expect(out).toContain('整体结构 OK')
  })
})
