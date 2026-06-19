import { renderToStaticMarkup } from 'react-dom/server'
import { readFileSync } from 'node:fs'
import { fileURLToPath } from 'node:url'
import { describe, expect, it } from 'vitest'
import SkillGraphDialog from './SkillGraphDialog'

describe('SkillGraphDialog', () => {
  it('presents skill pipeline orchestration as a standalone dialog', () => {
    const markup = renderToStaticMarkup(
      <SkillGraphDialog
        onClose={() => {}}
        targetRepo={null}
        sessionId={null}
        sessionRunning={false}
      />
    )

    expect(markup).toContain('Skill 编排')
    expect(markup).toContain('skill-manager-modal')
    expect(markup).toContain('请先选择项目')
    expect(markup).not.toContain('扫描本机 Skill')
    expect(markup).not.toContain('添加 Skill 目录')
  })

  it('passes the current project root when refreshing graph skills', () => {
    const source = readFileSync(
      fileURLToPath(new URL('./skill-graph/SkillGraphPanel.tsx', import.meta.url)),
      'utf8'
    )

    expect(source).toContain('window.api.habit.localSkills.scan({ targetRepo: targetRepo ?? null })')
  })
})
