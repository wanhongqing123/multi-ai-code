/**
 * Renderer-side mirror of the Skill types defined in
 * `electron/habit/skills.ts`. Kept in sync by hand because the renderer
 * isn't allowed to import from electron/ directly (it would bundle Node
 * deps into the web bundle).
 */

export type SkillStepType = 'prompt' | 'wait-response'

export interface PromptStep {
  type: 'prompt'
  text: string
}

export interface WaitResponseStep {
  type: 'wait-response'
  timeoutMs?: number
}

export type SkillStep = PromptStep | WaitResponseStep

export type SkillSource = 'manual' | 'candidate' | 'imported' | 'local'

export interface Skill {
  id: number | string
  name: string
  description: string | null
  trigger: string | null
  steps: SkillStep[]
  source: SkillSource | null
  candidateId: number | null
  enabled: boolean
  createdAt: number
  updatedAt: number
  lastUsedAt: number | null
}

const VAR_RE = /\{([A-Za-z0-9_一-龥]+)\}/g

export function collectSkillVariables(steps: SkillStep[]): string[] {
  const found = new Set<string>()
  for (const step of steps) {
    if (step.type !== 'prompt') continue
    const matches = step.text.matchAll(VAR_RE)
    for (const m of matches) found.add(m[1])
  }
  return Array.from(found)
}

export function substituteVariables(
  text: string,
  vars: Record<string, string>
): string {
  return text.replace(VAR_RE, (_, key: string) =>
    Object.prototype.hasOwnProperty.call(vars, key) ? vars[key] : `{${key}}`
  )
}
