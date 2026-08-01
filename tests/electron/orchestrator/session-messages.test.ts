import { describe, expect, it } from 'vitest'
import {
  formatAnnotationsForSession,
  formatInitialMessage,
  type SessionAnnotation
} from '../../../electron/orchestrator/session-messages.js'

describe('formatInitialMessage', () => {
  it('keeps startup message lightweight when a plan already exists', () => {
    const out = formatInitialMessage({
      planName: 'add-auth',
      planAbsPath: '/repo/.multi-ai-code/designs/add-auth.md',
      planExists: true
    })

    expect(out).toContain('add-auth')
    expect(out).toContain('/repo/.multi-ai-code/designs/add-auth.md')
    expect(out).toContain('请先阅读当前方案文件')
    expect(out).toContain('此时不要修改任何代码')
    expect(out).not.toContain('# 方案：增加 OAuth')
    expect(out).not.toContain('详细步骤')
  })

  it('returns a kick-off design message when the plan file does not exist', () => {
    const out = formatInitialMessage({
      planName: 'add-auth',
      planAbsPath: '/repo/.multi-ai-code/designs/add-auth.md',
      planExists: false
    })

    expect(out).toContain('add-auth')
    expect(out).toContain('/repo/.multi-ai-code/designs/add-auth.md')
    expect(out).toContain('澄清需求')
    expect(out).not.toContain('请先阅读当前方案文件')
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

  it('references each annotation with explicit file, line, snippet, and comment fields', () => {
    const out = formatAnnotationsForSession({
      annotations: [ann1],
      generalComment: '',
      planAbsPath: '/repo/.multi-ai-code/designs/add-auth.md'
    })
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
      generalComment: '整体结构 OK，改前加一层抽象。',
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
      comment: '移动到 <Router> 外层'
    }
    const out = formatAnnotationsForSession({
      annotations: [ann1, ann2],
      generalComment: '',
      planAbsPath: '/repo/.multi-ai-code/designs/add-auth.md'
    })
    const firstIdx = out.indexOf('文件: src/auth.ts')
    const secondIdx = out.indexOf('文件: src/app.tsx')
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

// 普通任务和定时任务只是「手动发」和「按点发」的区别，没有「当前选中任务」
// 这种状态。批注针对的是代码改动，因此方案文件是可选上下文，不是前置条件。
describe('formatAnnotationsForSession 无方案文件时', () => {
  const ann: SessionAnnotation = {
    file: 'src/auth.ts',
    lineRange: '10-12',
    snippet: 'const token = req.headers.auth',
    comment: '改为读取 Authorization Bearer'
  }

  it('still produces a usable annotation batch', () => {
    const out = formatAnnotationsForSession({ annotations: [ann], generalComment: '整体没问题' })

    expect(out.startsWith('# 用户批注')).toBe(true)
    expect(out).toContain('src/auth.ts')
    expect(out).toContain('改为读取 Authorization Bearer')
    expect(out).toContain('整体没问题')
  })

  // 不能出现空路径的残句（如「方案文档（``）」），那比不提更糟：会把模型
  // 引去找一个不存在的文件。注意正文本身有 ``` 代码围栏，不能笼统断言不含反引号。
  it('never mentions a plan document it does not have', () => {
    const out = formatAnnotationsForSession({ annotations: [ann], generalComment: '' })

    expect(out).not.toContain('方案文档')
    expect(out).not.toContain('（``）')
    expect(out).not.toContain('/ 方案')
    expect(out).toContain('直接修改代码')
    expect(out).toContain('请按照以上批注调整代码，完成后在终端里简述改了什么。')
  })

  it('still references the plan document when one is active', () => {
    const out = formatAnnotationsForSession({
      annotations: [ann],
      generalComment: '',
      planAbsPath: '/repo/.multi-ai-code/designs/add-auth.md'
    })

    expect(out).toContain('方案文档')
    expect(out).toContain('/repo/.multi-ai-code/designs/add-auth.md')
  })
})
