import { describe, expect, it } from 'vitest'
import { extractRemoteImAicliImagePaths } from './aicliImageOutput.js'

describe('remote IM AICLI image output parsing', () => {
  it('extracts markdown image paths and standalone local image paths once', () => {
    const text = [
      '截图如下：![desktop](/Users/me/MultiAICode/remote-im/images/desktop_shot.png)',
      '保存路径： /Users/me/MultiAICode/remote-im/images/desktop_shot.png',
      '另一个文件：`/tmp/result.webp`',
      '普通文件：/tmp/readme.txt'
    ].join('\n')

    expect(extractRemoteImAicliImagePaths(text)).toEqual([
      '/Users/me/MultiAICode/remote-im/images/desktop_shot.png',
      '/tmp/result.webp'
    ])
  })

  it('ignores remote URLs and unsupported local paths', () => {
    expect(
      extractRemoteImAicliImagePaths(
        '![remote](https://example.test/photo.png)\n本地文档 /tmp/report.md'
      )
    ).toEqual([])
  })
})
