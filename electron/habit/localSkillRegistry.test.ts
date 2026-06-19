import { mkdir, mkdtemp, writeFile } from 'fs/promises'
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
