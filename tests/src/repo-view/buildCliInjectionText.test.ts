import { describe, expect, it } from 'vitest'
import { buildCliInjectionText } from '../../../src/repo-view/buildCliInjectionText'

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

    // 分析缓存约定已删除：提示词不该再指示 AI 往仓库里写文件，
    // 否则「我们不建目录」只会变成「AI 自己建」。
    expect(text).not.toContain('## 记忆约定')
    expect(text).not.toContain('.multi-ai-code')
  })

  it('never asks the CLI to write an analysis cache file', () => {
    const text = buildCliInjectionText(baseInput)
    expect(text).not.toContain('.multi-ai-code')
    expect(text).not.toContain('分析缓存')
    expect(text).not.toContain('append')
  })
})
