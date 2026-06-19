import { mkdir, mkdtemp, symlink, writeFile } from 'fs/promises'
import { join } from 'path'
import { tmpdir } from 'os'
import { describe, expect, it } from 'vitest'
import {
  addLocalSkillSource,
  defaultLocalSkillRoots,
  scanLocalSkills,
  setLocalSkillEnabled
} from './localSkillRegistry.js'

async function writeSkill(dir: string, markdown: string): Promise<void> {
  await writeFile(join(dir, 'SKILL.md'), markdown, 'utf8')
}

describe('local skill registry', () => {
  it('discovers user-level Claude skills from .claude/skills by default', async () => {
    const home = await mkdtemp(join(tmpdir(), 'mac-local-claude-skills-'))
    const skillDir = join(home, '.claude', 'skills', 'daily-review')
    await mkdir(skillDir, { recursive: true })
    await writeSkill(
      skillDir,
      [
        '---',
        'name: daily-review',
        'description: Review daily work habits',
        '---',
        '',
        '# Daily Review'
      ].join('\n')
    )

    const snapshot = await scanLocalSkills({
      defaultRoots: defaultLocalSkillRoots(home),
      statePath: join(home, 'registry.json')
    })

    expect(snapshot.skills).toHaveLength(1)
    expect(snapshot.skills[0]).toMatchObject({
      name: 'daily-review',
      description: 'Review daily work habits',
      sourceName: 'Claude Skills',
      sourcePath: join(home, '.claude', 'skills')
    })
  })

  it('discovers project-level skills from the current repo roots', async () => {
    const root = await mkdtemp(join(tmpdir(), 'mac-local-project-skills-'))
    const projectRoot = join(root, 'target-repo')
    const sourceRoots = [
      {
        path: join(projectRoot, '.claude', 'skills'),
        name: 'project-claude',
        sourceName: 'Project Claude Skills'
      },
      {
        path: join(projectRoot, '.codex', 'skills'),
        name: 'project-codex',
        sourceName: 'Project Codex Skills'
      },
      {
        path: join(projectRoot, '.multi-ai-code', 'skills'),
        name: 'project-multi-ai',
        sourceName: 'Project Multi-AI Skills'
      }
    ]

    for (const source of sourceRoots) {
      const skillDir = join(source.path, source.name)
      await mkdir(skillDir, { recursive: true })
      await writeSkill(
        skillDir,
        [
          '---',
          `name: ${source.name}`,
          `description: ${source.sourceName}`,
          '---',
          '',
          `# ${source.name}`
        ].join('\n')
      )
    }

    const snapshot = await scanLocalSkills({
      defaultRoots: [],
      projectRoot,
      statePath: join(root, 'registry.json')
    })

    expect(snapshot.skills.map((skill) => skill.name).sort()).toEqual([
      'project-claude',
      'project-codex',
      'project-multi-ai'
    ])
    expect(snapshot.sources.filter((source) => source.skillCount > 0)).toEqual(
      sourceRoots.map((source) =>
        expect.objectContaining({
          name: source.sourceName,
          path: source.path,
          kind: 'project',
          skillCount: 1
        })
      )
    )
  })

  it('follows linked Claude skill directories', async () => {
    const root = await mkdtemp(join(tmpdir(), 'mac-local-linked-claude-skills-'))
    const realSkillDir = join(root, 'real-skills', 'apollo-git-flow')
    const claudeSkillsRoot = join(root, '.claude', 'skills')
    const linkedSkillDir = join(claudeSkillsRoot, 'apollo-git-flow')
    await mkdir(realSkillDir, { recursive: true })
    await mkdir(claudeSkillsRoot, { recursive: true })
    await writeSkill(
      realSkillDir,
      [
        '---',
        'name: apollo-git-flow',
        'description: Apollo git workflow',
        '---',
        '',
        '# Apollo Git Flow'
      ].join('\n')
    )
    await symlink(realSkillDir, linkedSkillDir, process.platform === 'win32' ? 'junction' : 'dir')

    const snapshot = await scanLocalSkills({
      defaultRoots: [claudeSkillsRoot],
      statePath: join(root, 'registry.json')
    })

    expect(snapshot.skills).toHaveLength(1)
    expect(snapshot.skills[0]).toMatchObject({
      name: 'apollo-git-flow',
      description: 'Apollo git workflow',
      dir: linkedSkillDir,
      sourcePath: claudeSkillsRoot
    })
  })

  it('keeps linked project source roots as separate sources', async () => {
    const root = await mkdtemp(join(tmpdir(), 'mac-linked-project-sources-'))
    const projectRoot = join(root, 'target-repo')
    const realSkillsRoot = join(root, 'real-project-skills')
    const realSkillDir = join(realSkillsRoot, 'shared-project-skill')
    const projectClaudeRoot = join(projectRoot, '.claude', 'skills')
    const projectCodexRoot = join(projectRoot, '.codex', 'skills')
    await mkdir(realSkillDir, { recursive: true })
    await mkdir(join(projectRoot, '.claude'), { recursive: true })
    await mkdir(join(projectRoot, '.codex'), { recursive: true })
    await writeSkill(
      realSkillDir,
      [
        '---',
        'name: shared-project-skill',
        'description: Shared project skill',
        '---',
        '',
        '# Shared Project Skill'
      ].join('\n')
    )
    await symlink(realSkillsRoot, projectClaudeRoot, process.platform === 'win32' ? 'junction' : 'dir')
    await symlink(realSkillsRoot, projectCodexRoot, process.platform === 'win32' ? 'junction' : 'dir')

    const snapshot = await scanLocalSkills({
      defaultRoots: [],
      projectRoot,
      statePath: join(root, 'registry.json')
    })

    expect(snapshot.skills.map((skill) => `${skill.sourceName}:${skill.name}`).sort()).toEqual([
      'Project Claude Skills:shared-project-skill',
      'Project Codex Skills:shared-project-skill'
    ])
    expect(
      snapshot.sources
        .filter((source) => source.skillCount > 0)
        .map((source) => `${source.name}:${source.skillCount}`)
    ).toEqual(['Project Claude Skills:1', 'Project Codex Skills:1'])
  })

  it('discovers SKILL.md packages from roots and parses metadata', async () => {
    const root = await mkdtemp(join(tmpdir(), 'mac-local-skills-'))
    const skillDir = join(root, 'claude-plugins-official', 'superpowers', '5.1.0', 'skills', 'brainstorming')
    await import('fs/promises').then((fs) => fs.mkdir(skillDir, { recursive: true }))
    await writeSkill(
      skillDir,
      [
        '---',
        'name: brainstorming',
        'description: Clarify requirements before implementation',
        'version: 5.1.0',
        '---',
        '',
        '# Brainstorming',
        'Use this before creative work.'
      ].join('\n')
    )

    const snapshot = await scanLocalSkills({
      defaultRoots: [join(root, 'claude-plugins-official')],
      statePath: join(root, 'registry.json')
    })

    expect(snapshot.skills).toHaveLength(1)
    expect(snapshot.skills[0]).toMatchObject({
      name: 'brainstorming',
      description: 'Clarify requirements before implementation',
      version: '5.1.0',
      enabled: true,
      health: 'ok'
    })
    expect(snapshot.sources[0]).toMatchObject({
      skillCount: 1,
      enabledCount: 1
    })
    expect(snapshot.totals).toEqual({ discovered: 1, enabled: 1, disabled: 0 })
  })

  it('persists custom source roots and enabled state', async () => {
    const root = await mkdtemp(join(tmpdir(), 'mac-local-skills-state-'))
    const customRoot = join(root, 'custom-skills')
    const customSkill = join(customRoot, 'verification')
    await import('fs/promises').then((fs) => fs.mkdir(customSkill, { recursive: true }))
    await writeSkill(
      customSkill,
      [
        '---',
        'name: verification-before-completion',
        'description: Verify before claiming completion',
        '---',
        '',
        '# Verification'
      ].join('\n')
    )
    const statePath = join(root, 'registry.json')

    await addLocalSkillSource(customRoot, { statePath })
    let snapshot = await scanLocalSkills({ defaultRoots: [], statePath })
    const found = snapshot.skills.find((skill) => skill.name === 'verification-before-completion')
    expect(found).toBeTruthy()
    expect(found?.enabled).toBe(true)

    await setLocalSkillEnabled(found!.id, false, { statePath })
    snapshot = await scanLocalSkills({ defaultRoots: [], statePath })
    expect(snapshot.skills.find((skill) => skill.id === found!.id)?.enabled).toBe(false)
    expect(snapshot.totals).toEqual({ discovered: 1, enabled: 0, disabled: 1 })
  })
})
