import { describe, it, expect } from 'vitest'
import { stageArtifactPath, renderTemplate } from './prompts.js'

describe('stageArtifactPath', () => {
  it('stage 1 with targetRepo returns absolute path under .multi-ai-code/designs', () => {
    expect(stageArtifactPath(1, 'my-plan', '/tmp/repo')).toBe(
      '/tmp/repo/.multi-ai-code/designs/my-plan.md'
    )
  })

  it('stage 1 with targetRepo and no label defaults to design.md', () => {
    expect(stageArtifactPath(1, null, '/tmp/repo')).toBe(
      '/tmp/repo/.multi-ai-code/designs/design.md'
    )
  })

  it('stage 1 sanitizes unsafe filename characters', () => {
    expect(stageArtifactPath(1, 'foo/bar:baz', '/tmp/repo')).toBe(
      '/tmp/repo/.multi-ai-code/designs/foo_bar_baz.md'
    )
  })

  it('stage 1 without targetRepo keeps legacy project-dir-relative path', () => {
    expect(stageArtifactPath(1, 'my-plan')).toBe(
      'workspaces/stage1_design/my-plan.md'
    )
  })

  it('stage 2-4 ignore targetRepo and return legacy relative path', () => {
    expect(stageArtifactPath(2, null, '/tmp/repo')).toBe('artifacts/impl-summary.md')
    expect(stageArtifactPath(3, null, '/tmp/repo')).toBe('artifacts/acceptance.md')
    expect(stageArtifactPath(4, null, '/tmp/repo')).toBe('artifacts/test-report.md')
  })
})

describe('renderTemplate planPending', () => {
  it('when targetRepo is set, uses <targetRepo>/.multi-ai-code/designs/<placeholder>.md', () => {
    const out = renderTemplate('ART={{ARTIFACT_PATH}}', {
      projectDir: '/p',
      artifactPath: 'ignored',
      planPending: true,
      targetRepo: '/tmp/repo'
    })
    expect(out).toBe(
      'ART=/tmp/repo/.multi-ai-code/designs/<你稍后将向用户询问得到的方案名称>.md'
    )
  })

  it('when absolute artifactPath is passed, keeps it as-is', () => {
    const out = renderTemplate('ART={{ARTIFACT_PATH}}', {
      projectDir: '/p',
      artifactPath: '/abs/path.md'
    })
    expect(out).toBe('ART=/abs/path.md')
  })
})
