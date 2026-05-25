import { describe, expect, it } from 'vitest'
import type { AggregatedCluster } from './aggregator.js'
import {
  buildGenerationPrompt,
  buildLocalHeuristicCandidate,
  parseGenerationResponse
} from './generator.js'

function mkCluster(overrides: Partial<AggregatedCluster> = {}): AggregatedCluster {
  return {
    id: 'test',
    kind: 'ai_prompt_main',
    sourceEventIds: [1, 2, 3],
    size: 3,
    representativeSamples: [
      'please explain the implementation of foo.ts',
      'please explain the implementation of bar.ts',
      'please explain the implementation of baz.ts'
    ],
    projectCount: 1,
    crossProject: false,
    firstTs: 1_700_000_000_000,
    lastTs: 1_700_000_000_000 + 7 * 24 * 60 * 60 * 1000,
    score: 3,
    ...overrides
  }
}

describe('buildGenerationPrompt: privacy invariants', () => {
  it('includes cluster metadata and up to 5 samples', () => {
    const prompt = buildGenerationPrompt(mkCluster())
    expect(prompt).toContain('重复次数: 3')
    expect(prompt).toContain('涉及项目数: 1')
    expect(prompt).toContain('foo.ts')
    expect(prompt).toContain('bar.ts')
    expect(prompt).toContain('baz.ts')
  })

  it('caps representative samples at 5', () => {
    const cluster = mkCluster({
      representativeSamples: Array.from({ length: 10 }, (_, i) => `sample line ${i + 1}`)
    })
    const prompt = buildGenerationPrompt(cluster)
    expect(prompt).toContain('sample line 1')
    expect(prompt).toContain('sample line 5')
    expect(prompt).not.toContain('sample line 6')
  })

  it('never embeds project_id, repo_path, or raw timestamp values', () => {
    const cluster = mkCluster({
      // Inputs that should NOT appear verbatim.
      firstTs: 9876543210000,
      lastTs: 9876543210999
    })
    const prompt = buildGenerationPrompt(cluster)
    expect(prompt).not.toContain('9876543210')
    expect(prompt).not.toContain('project_id')
    expect(prompt).not.toContain('repo_path')
  })
})

describe('parseGenerationResponse', () => {
  it('parses bare JSON with multi-step steps[]', () => {
    const r = parseGenerationResponse(
      '{"title":"看实现","trigger":"看实现","steps":[' +
        '{"type":"prompt","text":"帮我看看 {file} 的实现"},' +
        '{"type":"wait-response"}],"variables":["file"]}'
    )
    expect(r.ok).toBe(true)
    expect(r.template?.title).toBe('看实现')
    expect(r.template?.trigger).toBe('看实现')
    expect(r.template?.steps).toHaveLength(2)
    expect(r.template?.steps?.[0]).toMatchObject({ type: 'prompt' })
    expect(r.template?.steps?.[1]).toMatchObject({ type: 'wait-response' })
  })

  it('still accepts legacy single-body JSON by wrapping it as one prompt step', () => {
    const r = parseGenerationResponse(
      '{"title":"看实现","body":"帮我看看 {file} 的实现","variables":["file"]}'
    )
    expect(r.ok).toBe(true)
    expect(r.template?.steps).toHaveLength(1)
    expect(r.template?.steps?.[0]).toMatchObject({
      type: 'prompt',
      text: '帮我看看 {file} 的实现'
    })
    // Legacy body field is preserved for backward-compat consumers.
    expect(r.template?.body).toContain('{file}')
  })

  it('drops invalid step entries and rejects when nothing valid remains', () => {
    const r = parseGenerationResponse(
      '{"title":"x","steps":[{"type":"eval","cmd":"rm"},{"type":"prompt","text":""}]}'
    )
    expect(r.ok).toBe(false)
  })

  it('strips a ```json``` fence', () => {
    const r = parseGenerationResponse(
      '```json\n{"title":"看","steps":[{"type":"prompt","text":"帮我看 {x}"}]}\n```'
    )
    expect(r.ok).toBe(true)
    expect(r.template?.title).toBe('看')
  })

  it('extracts JSON when surrounded by chatter', () => {
    const r = parseGenerationResponse(
      'Sure! Here is the skill: {"title":"x","steps":[{"type":"prompt","text":"y"}]} hope this helps'
    )
    expect(r.ok).toBe(true)
    expect(r.template?.title).toBe('x')
  })

  it('rejects empty input', () => {
    expect(parseGenerationResponse('').ok).toBe(false)
  })

  it('rejects malformed JSON', () => {
    const r = parseGenerationResponse('not even close')
    expect(r.ok).toBe(false)
    expect(r.error).toBeDefined()
  })

  it('rejects JSON missing title', () => {
    const r = parseGenerationResponse('{"steps":[{"type":"prompt","text":"x"}]}')
    expect(r.ok).toBe(false)
  })

  it('rejects JSON with no steps and no body', () => {
    const r = parseGenerationResponse('{"title":"only title"}')
    expect(r.ok).toBe(false)
  })

  it('tags parsed template with source=cli', () => {
    const r = parseGenerationResponse(
      '{"title":"x","steps":[{"type":"prompt","text":"y"}]}'
    )
    expect(r.template?.meta?.source).toBe('cli')
  })
})

describe('buildLocalHeuristicCandidate', () => {
  it('wraps the longest sample into a single prompt step', () => {
    const cluster = mkCluster({
      representativeSamples: ['short', 'medium length one', 'this is by far the longest sample']
    })
    const c = buildLocalHeuristicCandidate(cluster)
    expect(c.steps).toHaveLength(1)
    expect(c.steps?.[0]).toMatchObject({
      type: 'prompt',
      text: 'this is by far the longest sample'
    })
  })

  it('truncates title to 18 chars + ellipsis when sample is long', () => {
    const cluster = mkCluster({
      representativeSamples: ['this is a very long sample that exceeds the title cap']
    })
    const c = buildLocalHeuristicCandidate(cluster)
    expect(c.title.length).toBeLessThanOrEqual(19) // 18 + ellipsis
    expect(c.title.endsWith('…')).toBe(true)
  })

  it('produces a non-empty trigger derived from the title', () => {
    const cluster = mkCluster({
      representativeSamples: ['这是一段够长的中文示例文本，用于触发字面截断']
    })
    const c = buildLocalHeuristicCandidate(cluster)
    expect(c.trigger).toBeDefined()
    expect(c.trigger?.length).toBeGreaterThan(0)
    expect(c.trigger?.length).toBeLessThanOrEqual(6)
  })

  it('handles empty samples gracefully', () => {
    const c = buildLocalHeuristicCandidate(mkCluster({ representativeSamples: [] }))
    expect(c.title).toContain('模板')
    expect(c.steps).toEqual([])
    expect(c.meta?.source).toBe('heuristic')
  })

  it('tags meta.source as heuristic', () => {
    const c = buildLocalHeuristicCandidate(mkCluster())
    expect(c.meta?.source).toBe('heuristic')
  })
})
