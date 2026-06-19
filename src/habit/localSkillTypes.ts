import type { Skill } from './skillTypes'

export type LocalSkillHealth = 'ok' | 'missing-file' | 'invalid'
export type LocalSkillSourceKind = 'default' | 'project' | 'custom'

export interface LocalSkillSource {
  id: string
  name: string
  path: string
  kind: LocalSkillSourceKind
  skillCount: number
  enabledCount: number
}

export interface LocalSkillPackage {
  id: string
  name: string
  description: string | null
  version: string | null
  dir: string
  skillFile: string
  sourceId: string
  sourceName: string
  sourcePath: string
  enabled: boolean
  health: LocalSkillHealth
  frontmatter: Record<string, string>
  markdown: string
  preview: string
  updatedAt: string | null
}

export interface LocalSkillSnapshot {
  sources: LocalSkillSource[]
  skills: LocalSkillPackage[]
  totals: {
    discovered: number
    enabled: number
    disabled: number
  }
  scannedAt: string
}

export function localSkillToRunnableSkill(skill: LocalSkillPackage): Skill {
  return {
    id: skill.id,
    name: skill.name,
    description: skill.description,
    trigger: null,
    source: 'local',
    candidateId: null,
    enabled: skill.enabled,
    createdAt: 0,
    updatedAt: skill.updatedAt ? Date.parse(skill.updatedAt) : 0,
    lastUsedAt: null,
    steps: [
      {
        type: 'prompt',
        text: [
          `使用本机 Skill：${skill.name}`,
          skill.description ? `描述：${skill.description}` : '',
          `来源路径：${skill.dir}`,
          '',
          '请严格按照下面 SKILL.md 的规则完成本步骤。',
          '',
          '--- SKILL.md ---',
          skill.markdown,
          '--- 输入 ---',
          '{input}'
        ]
          .filter(Boolean)
          .join('\n')
      },
      { type: 'wait-response' }
    ]
  }
}
