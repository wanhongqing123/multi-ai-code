import { describe, expect, it } from 'vitest'
import {
  collectSkillVariables,
  isValidStep,
  substituteVariables,
  type SkillStep
} from './skills.js'

describe('isValidStep', () => {
  it('accepts a non-empty prompt step', () => {
    expect(isValidStep({ type: 'prompt', text: 'hi' })).toBe(true)
  })

  it('rejects a prompt step with empty text', () => {
    expect(isValidStep({ type: 'prompt', text: '' })).toBe(false)
  })

  it('accepts a wait-response step with and without timeoutMs', () => {
    expect(isValidStep({ type: 'wait-response' })).toBe(true)
    expect(isValidStep({ type: 'wait-response', timeoutMs: 5000 })).toBe(true)
  })

  it('rejects wait-response with non-number timeout', () => {
    expect(
      isValidStep({ type: 'wait-response', timeoutMs: 'soon' as unknown as number })
    ).toBe(false)
  })

  it('rejects unknown step types', () => {
    expect(isValidStep({ type: 'eval', text: 'rm -rf' } as unknown)).toBe(false)
  })

  it('rejects non-objects', () => {
    expect(isValidStep(null)).toBe(false)
    expect(isValidStep('prompt')).toBe(false)
  })
})

describe('collectSkillVariables', () => {
  it('finds every distinct {var} across prompt steps', () => {
    const steps: SkillStep[] = [
      { type: 'prompt', text: '看一下 {file} 的实现' },
      { type: 'wait-response' },
      { type: 'prompt', text: '总结 {file} 里跟 {topic} 相关的部分' }
    ]
    expect(collectSkillVariables(steps).sort()).toEqual(['file', 'topic'])
  })

  it('returns empty list when no placeholders exist', () => {
    expect(collectSkillVariables([{ type: 'prompt', text: 'no vars here' }])).toEqual([])
  })

  it('captures Chinese-character variable names', () => {
    const steps: SkillStep[] = [
      { type: 'prompt', text: '请阅读 {文件} 并回答 {问题}' }
    ]
    expect(collectSkillVariables(steps).sort()).toEqual(['问题', '文件'].sort())
  })
})

describe('substituteVariables', () => {
  it('replaces known placeholders', () => {
    expect(
      substituteVariables('hello {name}, look at {file}', { name: 'Ada', file: 'a.ts' })
    ).toBe('hello Ada, look at a.ts')
  })

  it('keeps unknown placeholders literal', () => {
    expect(substituteVariables('{a} {b}', { a: 'A' })).toBe('A {b}')
  })

  it('substitutes the same placeholder multiple times', () => {
    expect(substituteVariables('{x} and {x}', { x: 'Y' })).toBe('Y and Y')
  })

  it('supports Chinese-character placeholders', () => {
    expect(substituteVariables('{文件}', { 文件: 'app.ts' })).toBe('app.ts')
  })

  it('does not interpret braces inside replaced values', () => {
    expect(substituteVariables('{a}', { a: '{b}' })).toBe('{b}')
  })
})
