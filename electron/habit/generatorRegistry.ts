import type { GenerateFn } from './scheduler.js'

/**
 * The current candidate generator. Default is a placeholder that skips
 * generation entirely (used until Step 7 wires the real CLI-backed generator).
 */
let currentGenerator: GenerateFn = async () => ({ skip: true })

export function setSkillGenerator(fn: GenerateFn): void {
  currentGenerator = fn
}

export function getSkillGenerator(): GenerateFn {
  return currentGenerator
}
