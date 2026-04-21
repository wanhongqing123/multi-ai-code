import { describe, it, expect, beforeEach, afterEach } from 'vitest'
import { promises as fs } from 'fs'
import { tmpdir } from 'os'
import { join } from 'path'
import {
  planArtifactPath,
  resolvePlanArtifactAbs,
  renderTemplate,
  mainCliArgs,
  MAIN_COMMAND_DEFAULT
} from './prompts.js'

describe('planArtifactPath', () => {
  it('returns <target_repo>/.multi-ai-code/designs/<label>.md when targetRepo is set', () => {
    expect(planArtifactPath('my-plan', '/tmp/repo')).toBe(
      '/tmp/repo/.multi-ai-code/designs/my-plan.md'
    )
  })

  it('defaults label to "design" when empty/null', () => {
    expect(planArtifactPath(null, '/tmp/repo')).toBe(
      '/tmp/repo/.multi-ai-code/designs/design.md'
    )
  })

  it('sanitizes unsafe filename characters', () => {
    expect(planArtifactPath('foo/bar:baz', '/tmp/repo')).toBe(
      '/tmp/repo/.multi-ai-code/designs/foo_bar_baz.md'
    )
  })

  it('returns undefined when targetRepo is missing', () => {
    expect(planArtifactPath('my-plan', null)).toBeUndefined()
    expect(planArtifactPath('my-plan', undefined)).toBeUndefined()
  })
})

describe('resolvePlanArtifactAbs', () => {
  let projectDir: string
  let targetRepo: string

  beforeEach(async () => {
    projectDir = await fs.mkdtemp(join(tmpdir(), 'mac-resolve-'))
    targetRepo = await fs.mkdtemp(join(tmpdir(), 'mac-repo-'))
    await fs.writeFile(
      join(projectDir, 'project.json'),
      JSON.stringify({ id: 'p', name: 'p', target_repo: targetRepo })
    )
  })
  afterEach(async () => {
    await fs.rm(projectDir, { recursive: true, force: true })
    await fs.rm(targetRepo, { recursive: true, force: true })
  })

  it('resolves to <target_repo>/.multi-ai-code/designs/<label>.md', async () => {
    const abs = await resolvePlanArtifactAbs(projectDir, 'my-plan')
    expect(abs).toBe(join(targetRepo, '.multi-ai-code', 'designs', 'my-plan.md'))
  })

  it('defaults label to design.md', async () => {
    const abs = await resolvePlanArtifactAbs(projectDir, null)
    expect(abs).toBe(join(targetRepo, '.multi-ai-code', 'designs', 'design.md'))
  })

  it('throws when project.json is missing', async () => {
    await fs.rm(join(projectDir, 'project.json'))
    await expect(resolvePlanArtifactAbs(projectDir, 'my-plan')).rejects.toThrow()
  })
})

describe('renderTemplate', () => {
  it('replaces all documented variables', () => {
    const out = renderTemplate(
      'P={{PROJECT_NAME}} R={{TARGET_REPO}} C={{STAGE_CWD}} D={{PROJECT_DIR}} A={{ARTIFACT_PATH}}',
      {
        projectDir: '/p',
        projectName: 'demo',
        targetRepo: '/repo',
        stageCwd: '/repo',
        artifactPath: '/repo/.multi-ai-code/designs/x.md'
      }
    )
    expect(out).toBe('P=demo R=/repo C=/repo D=/p A=/repo/.multi-ai-code/designs/x.md')
  })

  it('uses planPending placeholder when flag is set', () => {
    const out = renderTemplate('A={{ARTIFACT_PATH}}', {
      projectDir: '/p',
      artifactPath: 'ignored',
      planPending: true,
      targetRepo: '/repo'
    })
    expect(out).toBe('A=/repo/.multi-ai-code/designs/<你稍后将向用户询问得到的方案名称>.md')
  })

  it('resolves relative artifactPath against projectDir', () => {
    const out = renderTemplate('A={{ARTIFACT_PATH}}', {
      projectDir: '/p',
      artifactPath: 'artifacts/foo.md'
    })
    expect(out).toBe('A=/p/artifacts/foo.md')
  })
})

describe('mainCliArgs', () => {
  it('default binary is claude', () => {
    expect(MAIN_COMMAND_DEFAULT).toBe('claude')
  })

  it('claude produces permission-mode auto + allowlist', () => {
    const args = mainCliArgs('claude')
    expect(args).toContain('--permission-mode')
    expect(args).toContain('auto')
    expect(args).toContain('--allowedTools')
  })

  it('codex produces --full-auto', () => {
    expect(mainCliArgs('codex')).toEqual(['--full-auto'])
  })
})
