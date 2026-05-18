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
  it('parses bare JSON with title and body', () => {
    const r = parseGenerationResponse(
      '{"title":"看实现","body":"帮我看看 {file} 的实现","variables":["file"]}'
    )
    expect(r.ok).toBe(true)
    expect(r.template?.title).toBe('看实现')
    expect(r.template?.body).toContain('{file}')
  })

  it('strips a ```json``` fence', () => {
    const r = parseGenerationResponse(
      '```json\n{"title":"看","body":"帮我看 {x}","variables":["x"]}\n```'
    )
    expect(r.ok).toBe(true)
    expect(r.template?.title).toBe('看')
  })

  it('extracts JSON when surrounded by chatter', () => {
    const r = parseGenerationResponse(
      'Sure! Here is the template: {"title":"x","body":"y"} hope this helps'
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

  it('rejects JSON missing required fields', () => {
    const r = parseGenerationResponse('{"title":"only title"}')
    expect(r.ok).toBe(false)
  })

  it('tags parsed template with source=cli', () => {
    const r = parseGenerationResponse('{"title":"x","body":"y"}')
    expect(r.template?.meta?.source).toBe('cli')
  })
})

describe('buildLocalHeuristicCandidate', () => {
  it('uses the longest sample as the body', () => {
    const cluster = mkCluster({
      representativeSamples: ['short', 'medium length one', 'this is by far the longest sample']
    })
    const c = buildLocalHeuristicCandidate(cluster)
    expect(c.body).toBe('this is by far the longest sample')
  })

  it('truncates title to 18 chars + ellipsis when sample is long', () => {
    const cluster = mkCluster({
      representativeSamples: ['this is a very long sample that exceeds the title cap']
    })
    const c = buildLocalHeuristicCandidate(cluster)
    expect(c.title.length).toBeLessThanOrEqual(19) // 18 + ellipsis
    expect(c.title.endsWith('…')).toBe(true)
  })

  it('handles empty samples gracefully', () => {
    const c = buildLocalHeuristicCandidate(mkCluster({ representativeSamples: [] }))
    expect(c.title).toContain('模板')
    expect(c.meta?.source).toBe('heuristic')
  })

  it('tags meta.source as heuristic', () => {
    const c = buildLocalHeuristicCandidate(mkCluster())
    expect(c.meta?.source).toBe('heuristic')
  })
})
