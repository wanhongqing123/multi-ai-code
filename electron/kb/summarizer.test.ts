import { describe, expect, it } from 'vitest'
import {
  buildSummaryPrompt,
  mergeEvidence,
  parseSummaryResponse,
  type SummarySnapshot
} from './summarizer.js'

function snap(over: Partial<SummarySnapshot> = {}): SummarySnapshot {
  return {
    repoPath: '/r',
    repoName: 'demo',
    recentCommits: [],
    recentFiles: [],
    recentPrompts: [],
    existingHotTopics: [],
    existingDigest: '',
    ...over
  }
}

describe('buildSummaryPrompt: privacy + bounds', () => {
  it('includes the repo name but not the absolute repo path', () => {
    const p = buildSummaryPrompt(snap({ repoPath: '/Users/secret/work/myproj', repoName: 'myproj' }))
    expect(p).toContain('myproj')
    expect(p).not.toContain('/Users/secret')
  })

  it('caps commit list at 50 even when more are provided', () => {
    const commits = Array.from({ length: 100 }, (_, i) => ({
      hash: `h${i}`,
      ts: i,
      author: 'a',
      subject: `commit ${i}`
    }))
    const p = buildSummaryPrompt(snap({ recentCommits: commits }))
    expect(p).toContain('h0')
    expect(p).toContain('h49')
    expect(p).not.toContain('h50')
  })

  it('caps user prompts at 20 and truncates long prompts', () => {
    const longPrompt = 'A'.repeat(500)
    const prompts = Array.from({ length: 30 }, () => longPrompt)
    const p = buildSummaryPrompt(snap({ recentPrompts: prompts }))
    expect(p.match(/A{500}/g)).toBeNull() // truncation cap is < 500
    expect((p.match(/A{200,}/g) ?? []).length).toBeLessThanOrEqual(20)
  })

  it('omits sections that have no content', () => {
    const p = buildSummaryPrompt(snap())
    expect(p).not.toContain('[最近 commit')
    expect(p).not.toContain('[最近修改的文件]')
    expect(p).not.toContain('[用户最近问 AI')
  })

  it('asks for strict JSON output (no fences)', () => {
    const p = buildSummaryPrompt(snap())
    expect(p).toContain('严格 JSON')
    expect(p).toContain('"topics"')
    expect(p).toContain('"digest"')
  })

  it('echoes the existing digest into the prompt so the model has context', () => {
    const p = buildSummaryPrompt(snap({ existingDigest: '本项目是一个 Electron 桌面工具。' }))
    expect(p).toContain('本项目是一个 Electron 桌面工具')
  })

  it('lists existing hot topics so the model avoids duplicates', () => {
    const p = buildSummaryPrompt(
      snap({
        existingHotTopics: [
          { topic: '认证系统', summary: '基于 OAuth2 的登录流。' }
        ]
      })
    )
    expect(p).toContain('认证系统')
    expect(p).toContain('OAuth2')
  })
})

describe('parseSummaryResponse', () => {
  it('parses a well-formed response with topics + digest', () => {
    const r = parseSummaryResponse(
      '{"topics":[{"topic":"auth","summary":"OAuth flow","importance":0.7,"evidence":{"commits":["a1"],"files":["src/auth.ts"]}}],"digest":"hello"}'
    )
    expect(r.ok).toBe(true)
    expect(r.topics).toHaveLength(1)
    expect(r.topics![0].topic).toBe('auth')
    expect(r.topics![0].importance).toBe(0.7)
    expect(r.topics![0].evidence?.commits).toEqual(['a1'])
    expect(r.digest).toBe('hello')
  })

  it('clamps importance into [0, 1]', () => {
    const r = parseSummaryResponse(
      '{"topics":[{"topic":"x","summary":"y","importance":2.5}],"digest":""}'
    )
    expect(r.topics![0].importance).toBe(1)
    const r2 = parseSummaryResponse(
      '{"topics":[{"topic":"x","summary":"y","importance":-1}],"digest":""}'
    )
    expect(r2.topics![0].importance).toBe(0)
  })

  it('drops topic entries missing required fields', () => {
    const r = parseSummaryResponse(
      '{"topics":[{"topic":"","summary":"y"},{"topic":"ok","summary":""},{"topic":"good","summary":"yes"}],"digest":"d"}'
    )
    expect(r.topics).toHaveLength(1)
    expect(r.topics![0].topic).toBe('good')
  })

  it('strips fenced JSON output', () => {
    const r = parseSummaryResponse(
      '```json\n{"topics":[],"digest":"hello"}\n```'
    )
    expect(r.ok).toBe(true)
    expect(r.digest).toBe('hello')
  })

  it('extracts JSON when surrounded by chatter', () => {
    const r = parseSummaryResponse(
      'Sure, here it is: {"topics":[],"digest":"ok"} hope this works.'
    )
    expect(r.ok).toBe(true)
    expect(r.digest).toBe('ok')
  })

  it('rejects empty response', () => {
    expect(parseSummaryResponse('').ok).toBe(false)
  })

  it('rejects content that has neither topics nor digest', () => {
    expect(parseSummaryResponse('{"topics":[]}').ok).toBe(false)
  })

  it('tolerates unknown extra fields without failing', () => {
    const r = parseSummaryResponse(
      '{"topics":[{"topic":"x","summary":"y"}],"digest":"d","extra":"ignored"}'
    )
    expect(r.ok).toBe(true)
  })
})

describe('mergeEvidence', () => {
  it('unions string arrays and de-duplicates', () => {
    const m = mergeEvidence(
      { commits: ['a', 'b'], files: ['x'] },
      { commits: ['b', 'c'], files: ['x', 'y'] }
    )
    expect(m.commits?.sort()).toEqual(['a', 'b', 'c'])
    expect(m.files?.sort()).toEqual(['x', 'y'])
  })

  it('preserves number arrays for prompt_ids', () => {
    const m = mergeEvidence({ prompt_ids: [1, 2] }, { prompt_ids: [2, 3] })
    expect(m.prompt_ids?.sort()).toEqual([1, 2, 3])
  })

  it('returns undefined for fields neither side supplies', () => {
    const m = mergeEvidence({}, {})
    expect(m.commits).toBeUndefined()
    expect(m.files).toBeUndefined()
    expect(m.prompt_ids).toBeUndefined()
  })

  it('caps each merged array at 30 entries', () => {
    const a = Array.from({ length: 50 }, (_, i) => `h${i}`)
    const b = Array.from({ length: 50 }, (_, i) => `j${i}`)
    const m = mergeEvidence({ commits: a }, { commits: b })
    expect(m.commits?.length).toBe(30)
  })
})
