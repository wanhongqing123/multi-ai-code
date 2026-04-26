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

  it('truncates very long paths and appends an 8-char hash suffix', () => {
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
    annotations: [
      {
        id: 'a1',
        filePath: 'libobs/obs-audio-controls.c',
        lineRange: '52-53',
        snippet: 'float cur_db;\nbool ignore_next_signal;',
        comment: '这行是什么意思？'
      }
    ],
    question: ''
  }

  it('emits single-file references, comments, and the default question', () => {
    const text = buildCliInjectionText(baseInput)
    expect(text).toContain('仓库根: /repo/obs-studio')
    expect(text).toContain('文件: libobs/obs-audio-controls.c')
    expect(text).toContain('## 标注 1（libobs/obs-audio-controls.c 第 52-53 行）')
    expect(text).toContain('代码片段：')
    expect(text).toContain('float cur_db;')
    expect(text).toContain('bool ignore_next_signal;')
    expect(text).toContain('说明: 这行是什么意思？')
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

  it('numbers multiple annotations in order across files', () => {
    const text = buildCliInjectionText({
      ...baseInput,
      annotations: [
        { ...baseInput.annotations[0], id: 'a1' },
        {
          id: 'a2',
          filePath: 'plugins/win-capture/game-capture.c',
          lineRange: '60',
          snippet: 'return 0;',
          comment: '这里返回什么？'
        }
      ]
    })
    expect(text).toContain('## 标注 1（libobs/obs-audio-controls.c 第 52-53 行）')
    expect(text).toContain('## 标注 2（plugins/win-capture/game-capture.c 第 60 行）')
    expect(text).toContain('文件数: 2')
    expect(text).toContain('- libobs/obs-audio-controls.c')
    expect(text).toContain('- plugins/win-capture/game-capture.c')
  })

  it('uses a multi-file cache path when annotations span multiple files', () => {
    const text = buildCliInjectionText({
      ...baseInput,
      annotations: [
        { ...baseInput.annotations[0], id: 'a1' },
        {
          id: 'a2',
          filePath: 'plugins/win-capture/game-capture.c',
          lineRange: '60',
          snippet: 'return 0;',
          comment: '这里返回什么？'
        }
      ]
    })

    expect(text).toContain('## 记忆约定')
    expect(text).toContain('.multi-ai-code/repo-view/analyses/__multi-file__.md')
  })

  it('keeps the single-file cache path for a single-file request', () => {
    const text = buildCliInjectionText(baseInput)
    expect(text).toContain(
      '.multi-ai-code/repo-view/analyses/libobs__obs-audio-controls.c.md'
    )
  })
})
