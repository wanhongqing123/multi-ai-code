import { readFileSync } from 'node:fs'
import { fileURLToPath } from 'node:url'
import { describe, expect, it } from 'vitest'

describe('SkillLibraryPanel structure', () => {
  it('opens skill details from the skill card context menu instead of a persistent right pane', () => {
    const source = readFileSync(
      fileURLToPath(new URL('./SkillLibraryPanel.tsx', import.meta.url)),
      'utf8'
    )

    expect(source).toContain('onContextMenu')
    expect(source).toContain('skill-manager-detail-popover')
    expect(source).toContain('SkillMarkdownPreview')
    expect(source).not.toContain('<aside className="skill-manager-detail">')
    expect(source).not.toContain('<pre>{detailSkill.preview || detailSkill.markdown')
    expect(source).not.toContain('window.api.habit.localSkills.openPath')
    expect(source).not.toContain('window.api.cc.sendUser')
    expect(source).not.toContain('buildApplySkillsPrompt')
    expect(source).not.toContain('打开目录')
    expect(source).not.toContain('查看文件')
    expect(source).not.toContain('toggleSkill(detailSkill)')
  })

  it('passes the current project root when scanning local skills', () => {
    const source = readFileSync(
      fileURLToPath(new URL('./SkillLibraryPanel.tsx', import.meta.url)),
      'utf8'
    )

    expect(source).toContain('targetRepo: string | null')
    expect(source).toContain('window.api.habit.localSkills.scan({ targetRepo: targetRepo ?? null })')
    expect(source).toContain('window.api.habit.localSkills.setEnabled(skill.id, !skill.enabled, {')
  })
})
