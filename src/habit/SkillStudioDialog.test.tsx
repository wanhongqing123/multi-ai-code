import { renderToStaticMarkup } from 'react-dom/server'
import { describe, expect, it } from 'vitest'
import SkillStudioDialog from './SkillStudioDialog'

describe('SkillStudioDialog', () => {
  it('presents skill management without graph or habit learning controls', () => {
    const markup = renderToStaticMarkup(
      <SkillStudioDialog
        onClose={() => {}}
        sessionId={null}
        sessionRunning={false}
        onSkillsChanged={() => {}}
      />
    )

    expect(markup).toContain('Skill')
    expect(markup).toContain('skill-manager-modal')
    expect(markup).toContain('skill-manager-search')
    expect(markup).toContain('全部启用')
    expect(markup).toContain('全部禁用')
    expect(markup).toContain('后续通过 Multi-AI Code 发送给 AICLI')
    expect(markup).not.toContain('发现的 Skills')
    expect(markup).not.toContain('本机 Skill 注册表')
    expect(markup).not.toContain('扫描本机 Skill')
    expect(markup).not.toContain('添加 Skill 目录')
    expect(markup).not.toContain('Skill 编排')
    expect(markup).not.toContain('候选')
    expect(markup).not.toContain('采集')
    expect(markup).not.toContain('学习')
    expect(markup).not.toContain('新建')
    expect(markup).not.toContain('添加步骤')
  })

  it('does not expose a standalone apply-to-AICLI command', () => {
    const markup = renderToStaticMarkup(
      <SkillStudioDialog
        onClose={() => {}}
        sessionId="session-1"
        sessionRunning={true}
        onSkillsChanged={() => {}}
      />
    )

    const applyBar = markup.match(/<div class="skill-manager-apply-bar">[\s\S]*?<\/div>/)?.[0] ?? ''
    expect(applyBar).toContain('每条消息')
    expect(applyBar).not.toContain('button')
    expect(applyBar).not.toContain('应用到 AICLI')
    expect(applyBar).not.toContain('清空禁用配置')
  })
})
