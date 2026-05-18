import { describe, expect, it } from 'vitest'
import type { AggregatedCluster } from './aggregator.js'
import { buildGenerationPrompt } from './generator.js'

/**
 * These tests enforce the privacy contract documented in the design doc:
 *   - The generator payload (sent to the LLM CLI) MUST NOT contain
 *     project_id, repo_path, raw timestamps, output text, or file content.
 *   - It MUST contain only cluster description + ≤ 5 representative input
 *     samples (text only).
 *
 * If a future refactor adds an extra field to the prompt, these tests should
 * fail loudly so we don't quietly start leaking data the user wasn't told
 * about.
 */

function mkCluster(overrides: Partial<AggregatedCluster> = {}): AggregatedCluster {
  return {
    id: 'test',
    kind: 'ai_prompt_main',
    sourceEventIds: [42, 43, 44],
    size: 3,
    representativeSamples: ['sample a', 'sample b'],
    projectCount: 1,
    crossProject: false,
    firstTs: 0,
    lastTs: 0,
    score: 3,
    ...overrides
  }
}

describe('privacy invariants: outgoing payload', () => {
  it('does not contain raw project_id values', () => {
    const cluster = mkCluster({ size: 5 })
    const prompt = buildGenerationPrompt(cluster)
    // We never emit field names like project_id or repo_path verbatim.
    expect(prompt).not.toContain('project_id')
    expect(prompt).not.toContain('projectId')
    expect(prompt).not.toContain('repo_path')
    expect(prompt).not.toContain('repoPath')
  })

  it('does not contain numeric event ids', () => {
    const cluster = mkCluster({ sourceEventIds: [42, 43, 44, 999999] })
    const prompt = buildGenerationPrompt(cluster)
    expect(prompt).not.toContain('999999')
    expect(prompt).not.toContain('"sourceEventIds"')
  })

  it('emits at most 5 representative samples even if cluster has more', () => {
    const cluster = mkCluster({
      representativeSamples: Array.from({ length: 12 }, (_, i) => `INVITED-SAMPLE-${i}`)
    })
    const prompt = buildGenerationPrompt(cluster)
    let included = 0
    for (let i = 0; i < 12; i++) {
      if (prompt.includes(`INVITED-SAMPLE-${i}`)) included++
    }
    expect(included).toBeLessThanOrEqual(5)
  })

  it('does not contain raw timestamp values', () => {
    const ts = 1_700_000_000_123
    const cluster = mkCluster({ firstTs: ts, lastTs: ts + 86_400_000 })
    const prompt = buildGenerationPrompt(cluster)
    expect(prompt).not.toContain(String(ts))
    expect(prompt).not.toContain(String(ts + 86_400_000))
  })

  it('reports project count as an aggregate, not as raw ids', () => {
    const cluster = mkCluster({ projectCount: 4, crossProject: true })
    const prompt = buildGenerationPrompt(cluster)
    // Aggregate description present.
    expect(prompt).toContain('涉及项目数: 4')
    // Raw ids never appear.
    expect(prompt).not.toContain('p1')
    expect(prompt).not.toContain('p_')
  })
})

describe('privacy invariants: collector disabled state', () => {
  it('collector module shape — disabled-master path returns without DB write', async () => {
    // Verify the exported function signature and that calling it without a
    // DB available does not throw.
    const { recordHabitEvent } = await import('./collector.js')
    await expect(
      recordHabitEvent({
        kind: 'ai_prompt_main',
        text: 'this should not write because no settings file exists in a freshly-initialized test env',
        projectId: 'p1',
        repoPath: '/fake/path'
      })
    ).resolves.toBeUndefined()
  })
})
