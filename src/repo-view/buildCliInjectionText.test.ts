import { describe, expect, it } from 'vitest'
import {
  buildCliInjectionText,
  encodeAnalysisFileName
} from './buildCliInjectionText'

describe('encodeAnalysisFileName', () => {
  it('replaces path separators with double underscore and appends .md', () => {
    expect(encodeAnalysisFileName('libobs/obs-audio-controls.c')).toBe(
      'libobs__obs-audio-controls.c.md'
    )
  })

  it('keeps a top-level filename intact', () => {
    expect(encodeAnalysisFileName('CMakeLists.txt')).toBe('CMakeLists.txt.md')
  })

  it('truncates very long paths and appends an 8-char sha1 suffix', () => {
    const deep = Array.from({ length: 40 }, (_, i) => `seg${i}`).join('/')
    const out = encodeAnalysisFileName(`${deep}/file.ts`)
    expect(out.length).toBeLessThanOrEqual(200)
    expect(out.endsWith('.md')).toBe(true)
    expect(out).toMatch(/__[0-9a-f]{8}\.md$/)
  })
})

describe('buildCliInjectionText', () => {
  const baseInput = {
    repoRoot: '/repo/obs-studio',
    filePath: 'libobs/obs-audio-controls.c',
    annotations: [
      {
        id: 'a1',
        filePath: 'libobs/obs-audio-controls.c',
        lineRange: '52-53',
        snippet: 'float cur_db;\nbool ignore_next_signal;',
        comment: '这行是什么意思'
      }
    ],
    question: ''
  }

  it('emits the file header, fenced snippet, comment, and default question', () => {
    const text = buildCliInjectionText(baseInput)
    expect(text).toContain('仓库根: /repo/obs-studio')
    expect(text).toContain('文件: libobs/obs-audio-controls.c')
    expect(text).toContain('## 标注 1（第 52-53 行）')
    expect(text).toContain('```c')
    expect(text).toContain('float cur_db;')
    expect(text).toContain('说明: 这行是什么意思')
    expect(text).toContain('## 问题')
    expect(text).toContain('请按标注分析')
  })

  it('uses the user-provided question instead of the default', () => {
    const text = buildCliInjectionText({
      ...baseInput,
      question: '主流程是什么？'
    })
    expect(text).toContain('主流程是什么？')
    expect(text).not.toContain('请按标注分析')
  })

  it('numbers multiple annotations in order', () => {
    const text = buildCliInjectionText({
      ...baseInput,
      annotations: [
        { ...baseInput.annotations[0], id: 'a1' },
        {
          id: 'a2',
          filePath: 'libobs/obs-audio-controls.c',
          lineRange: '60',
          snippet: 'return 0;',
          comment: '这里返回什么'
        }
      ]
    })
    expect(text).toContain('## 标注 1（第 52-53 行）')
    expect(text).toContain('## 标注 2（第 60 行）')
  })

  it('emits a 记忆约定 section pointing at the encoded cache path', () => {
    const text = buildCliInjectionText(baseInput)
    expect(text).toContain('## 记忆约定')
    expect(text).toContain(
      '.multi-ai-code/repo-view/analyses/libobs__obs-audio-controls.c.md'
    )
    expect(text).toContain('先读取并尽量复用既有结论')
    expect(text).toContain('append 形式写入该文件')
  })

  it('picks a fence language from the file extension and falls back to empty', () => {
    const tsx = buildCliInjectionText({
      ...baseInput,
      filePath: 'src/App.tsx',
      annotations: [
        { ...baseInput.annotations[0], filePath: 'src/App.tsx' }
      ]
    })
    expect(tsx).toContain('```tsx')

    const unknown = buildCliInjectionText({
      ...baseInput,
      filePath: 'data/blob.xyz',
      annotations: [
        { ...baseInput.annotations[0], filePath: 'data/blob.xyz' }
      ]
    })
    // unknown extension → bare ``` fence
    expect(unknown).toMatch(/```\nfloat cur_db;/)
  })
})
