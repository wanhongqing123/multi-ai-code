import { describe, expect, it } from 'vitest'
import type { LocalSkillPackage, LocalSkillSnapshot } from './localSkillRegistry.js'
import { buildSkillAvailabilityContext, decorateUserMessageWithSkillContext } from './skillConversationContext.js'

function skill(overrides: Partial<LocalSkillPackage>): LocalSkillPackage {
  return {
    id: 'skill_1',
    name: 'brainstorming',
    description: 'Clarify requirements before implementation',
    version: null,
    dir: 'C:\\Users\\demo\\.claude\\plugins\\cache\\superpowers\\skills\\brainstorming',
    skillFile: 'C:\\Users\\demo\\.claude\\plugins\\cache\\superpowers\\skills\\brainstorming\\SKILL.md',
    sourceId: 'source_1',
    sourceName: 'Claude Skills',
    sourcePath: 'C:\\Users\\demo\\.claude\\plugins\\cache',
    enabled: true,
    health: 'ok',
    frontmatter: {},
    markdown: '# Skill',
    preview: '# Skill',
    updatedAt: null,
    ...overrides
  }
}

function snapshot(skills: LocalSkillPackage[]): LocalSkillSnapshot {
  return {
    sources: [],
    skills,
    totals: {
      discovered: skills.length,
      enabled: skills.filter((item) => item.enabled).length,
      disabled: skills.filter((item) => !item.enabled).length
    },
    scannedAt: '2026-06-17T00:00:00.000Z'
  }
}

describe('skill conversation context', () => {
  it('builds availability context with enabled and disabled skill groups', () => {
    const context = buildSkillAvailabilityContext(snapshot([
      skill({ name: 'brainstorming', enabled: true }),
      skill({
        id: 'skill_2',
        name: 'skill-creator',
        description: 'Create effective skills',
        enabled: false,
        sourceName: 'Codex Skills',
        skillFile: 'C:\\Users\\demo\\.codex\\skills\\skill-creator\\SKILL.md'
      })
    ]))

    expect(context).toContain('Multi-AI Code Skill 可用性')
    expect(context).toContain('可以使用的 Skills:')
    expect(context).toContain('brainstorming')
    expect(context).toContain('不可以使用的 Skills:')
    expect(context).toContain('skill-creator')
    expect(context).toContain('不要主动使用、加载或触发')
  })

  it('does not inject skill availability into outgoing user messages', () => {
    const decorated = decorateUserMessageWithSkillContext(
      '请帮我检查这个项目',
      snapshot([skill({ name: 'brainstorming', enabled: true })])
    )

    expect(decorated).toBe('请帮我检查这个项目')
    expect(decorated).not.toContain('Multi-AI Code Skill 可用性')
    expect(decorated).not.toContain('brainstorming')
    expect(decorated).not.toContain('<用户消息>')
  })

  it('leaves user messages unchanged when no skills are discovered', () => {
    expect(decorateUserMessageWithSkillContext('你好', snapshot([]))).toBe('你好')
  })
})
