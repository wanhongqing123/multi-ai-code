import type { LocalSkillPackage, LocalSkillSnapshot } from './localSkillRegistry.js'

function formatSkillList(skills: LocalSkillPackage[]): string[] {
  if (skills.length === 0) return ['- 无']
  return skills.map((skill, index) =>
    [
      `${index + 1}. ${skill.name}`,
      skill.description ? `   - 描述: ${skill.description}` : '',
      `   - 来源: ${skill.sourceName}`,
      `   - 文件: ${skill.skillFile}`
    ]
      .filter(Boolean)
      .join('\n')
  )
}

export function buildSkillAvailabilityContext(snapshot: LocalSkillSnapshot): string {
  if (snapshot.skills.length === 0) return ''

  const enabledSkills = snapshot.skills.filter((skill) => skill.enabled)
  const disabledSkills = snapshot.skills.filter((skill) => !skill.enabled)

  return [
    '<Multi-AI Code Skill 可用性>',
    '这是 Multi-AI Code 在每次用户消息前注入的 Skill 可用性上下文，不是用户的新任务。',
    '模型只能主动选择、触发或加载“可以使用”的 Skill；“不可以使用”的 Skill 不要主动使用、加载或触发，除非用户明确要求重新启用。',
    '',
    '可以使用的 Skills:',
    ...formatSkillList(enabledSkills),
    '',
    '不可以使用的 Skills:',
    ...formatSkillList(disabledSkills),
    '</Multi-AI Code Skill 可用性>'
  ].join('\n')
}

export function decorateUserMessageWithSkillContext(
  userMessage: string,
  _snapshot: LocalSkillSnapshot
): string {
  return userMessage
}
