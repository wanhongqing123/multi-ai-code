import {
  findKbEntryByTopic,
  insertKbEntry,
  listKbEntries,
  rowToEntry,
  updateKbEntry,
  upsertKbMeta,
  type KbEntry,
  type KbEvidence
} from './db.js'
import type { GitCommit, RecentFile } from './sources.js'

/**
 * Snapshot of everything the summarizer sends to the CLI for one run.
 * Built by the scheduler and handed in here. Tested as a pure value object.
 */
export interface SummarySnapshot {
  repoPath: string
  /** Short repo name (basename of repoPath) — included in the prompt header. */
  repoName: string
  recentCommits: GitCommit[]
  recentFiles: RecentFile[]
  /** Up to N most-recent user prompts that ran on the main session. */
  recentPrompts: string[]
  /** hot tier entries already in the KB — used so the model knows what NOT to re-add. */
  existingHotTopics: { topic: string; summary: string }[]
  /** Cumulative digest so the model has long-term context. May be empty. */
  existingDigest: string
}

/**
 * Builds the structured prompt sent to the CLI subprocess. Privacy-critical:
 * we do NOT include AI replies, repo absolute path, or raw file contents.
 * Only: short commit subjects, file path *names* (not their content), user
 * prompt text the user already typed (already collected by habit), and
 * topic-level summaries we previously generated.
 */
export function buildSummaryPrompt(snap: SummarySnapshot): string {
  const lines: string[] = []
  lines.push('我是一个 AI 工作平台，正在帮用户增量维护这个项目的本地知识库。')
  lines.push('请基于下面的素材，给出一个结构化的 JSON 摘要：')
  lines.push('')
  lines.push(`[项目]`)
  lines.push(`- 名称: ${snap.repoName}`)
  if (snap.existingDigest.trim()) {
    lines.push('')
    lines.push('[既有摘要（保留有用部分，只在确有新信息时改写）]')
    lines.push(snap.existingDigest.trim().slice(0, 1500))
  }
  if (snap.existingHotTopics.length > 0) {
    lines.push('')
    lines.push('[当前热区主题（避免重复创建，相同主题请增量更新）]')
    for (const t of snap.existingHotTopics.slice(0, 20)) {
      lines.push(`- ${t.topic}: ${t.summary.slice(0, 200)}`)
    }
  }
  if (snap.recentCommits.length > 0) {
    lines.push('')
    lines.push('[最近 commit (短哈希 | 主题)]')
    for (const c of snap.recentCommits.slice(0, 50)) {
      lines.push(`- ${c.hash} | ${c.subject}`)
    }
  }
  if (snap.recentFiles.length > 0) {
    lines.push('')
    lines.push('[最近修改的文件]')
    for (const f of snap.recentFiles.slice(0, 30)) {
      lines.push(`- ${f.relPath}`)
    }
  }
  if (snap.recentPrompts.length > 0) {
    lines.push('')
    lines.push('[用户最近问 AI 的问题（节选）]')
    for (const p of snap.recentPrompts.slice(0, 20)) {
      lines.push(`- ${truncate(p, 200)}`)
    }
  }
  lines.push('')
  lines.push('[要求]')
  lines.push('只返回严格 JSON，不要 Markdown 代码围栏，结构如下：')
  lines.push('{')
  lines.push('  "topics": [')
  lines.push(
    '    {"topic":"主题名","summary":"1-3 句话核心信息","evidence":{"commits":["hash1"],"files":["path"],"prompt_ids":[]},"importance":0.0-1.0}'
  )
  lines.push('  ],')
  lines.push('  "digest": "≤ 800 字的项目整体概述 + 当前关注点 + 最近主题"')
  lines.push('}')
  lines.push('')
  lines.push('指导原则：')
  lines.push('- topics 数量 0-8 条；只产出真正新的或确实有新进展的主题')
  lines.push('- 不要重复 "[当前热区主题]" 中已有且没有新信息的主题')
  lines.push('- importance 估个 0-1：核心架构/反复出现 → 0.7+；零星细节 → 0.3-')
  lines.push('- digest 用中文一段写，包含项目是干啥的、最近在做什么、有哪些主要模块')
  return lines.join('\n')
}

function truncate(s: string, max: number): string {
  const trimmed = s.replace(/\s+/g, ' ').trim()
  return trimmed.length > max ? trimmed.slice(0, max) + '…' : trimmed
}

const FENCE_RE = /```(?:json)?\s*([\s\S]*?)\s*```/i

export interface ParsedSummary {
  ok: boolean
  topics?: Array<{
    topic: string
    summary: string
    evidence?: KbEvidence
    importance?: number
  }>
  digest?: string
  error?: string
}

/**
 * Tolerant JSON parser — same shape as the habit generator parser: handles
 * fenced output, surrounding chatter, and unknown extra fields.
 */
export function parseSummaryResponse(raw: string): ParsedSummary {
  if (!raw || typeof raw !== 'string') return { ok: false, error: 'empty response' }
  let candidate = raw.trim()
  const fenced = candidate.match(FENCE_RE)
  if (fenced && fenced[1]) candidate = fenced[1].trim()
  if (!candidate.startsWith('{')) {
    const first = candidate.indexOf('{')
    const last = candidate.lastIndexOf('}')
    if (first >= 0 && last > first) candidate = candidate.slice(first, last + 1)
  }
  let parsed: unknown
  try {
    parsed = JSON.parse(candidate)
  } catch (err) {
    return { ok: false, error: `failed to parse JSON: ${(err as Error).message}` }
  }
  if (!parsed || typeof parsed !== 'object') {
    return { ok: false, error: 'response is not an object' }
  }
  const obj = parsed as Record<string, unknown>
  const digest = typeof obj.digest === 'string' ? obj.digest.trim() : ''
  const topicsRaw = Array.isArray(obj.topics) ? obj.topics : []
  const topics: ParsedSummary['topics'] = []
  for (const tRaw of topicsRaw) {
    if (!tRaw || typeof tRaw !== 'object') continue
    const t = tRaw as Record<string, unknown>
    const topic = typeof t.topic === 'string' ? t.topic.trim() : ''
    const summary = typeof t.summary === 'string' ? t.summary.trim() : ''
    if (!topic || !summary) continue
    const evidence: KbEvidence = {}
    if (t.evidence && typeof t.evidence === 'object') {
      const ev = t.evidence as Record<string, unknown>
      if (Array.isArray(ev.commits)) {
        evidence.commits = ev.commits.filter((x): x is string => typeof x === 'string')
      }
      if (Array.isArray(ev.files)) {
        evidence.files = ev.files.filter((x): x is string => typeof x === 'string')
      }
      if (Array.isArray(ev.prompt_ids)) {
        evidence.prompt_ids = ev.prompt_ids.filter(
          (x): x is number => typeof x === 'number'
        )
      }
    }
    const importance =
      typeof t.importance === 'number' && Number.isFinite(t.importance)
        ? Math.max(0, Math.min(1, t.importance))
        : 0.5
    topics.push({ topic, summary, evidence, importance })
  }
  if (topics.length === 0 && !digest) {
    return { ok: false, error: 'response has no usable content' }
  }
  return { ok: true, topics, digest }
}

/**
 * Writes the parsed summary into the KB:
 *   - merges into existing entries by topic name (case-insensitive)
 *   - inserts new ones into the `hot` tier
 *   - persists the digest into kb_meta
 *
 * Returns the number of entries created vs updated for telemetry.
 */
export function applySummaryToKb(
  repoPath: string,
  parsed: ParsedSummary
): { created: number; updated: number; digest: string } {
  let created = 0
  let updated = 0
  if (parsed.ok && parsed.topics) {
    for (const t of parsed.topics) {
      const existing = findKbEntryByTopic(repoPath, t.topic)
      if (existing) {
        updateKbEntry(existing.id, {
          summary: t.summary,
          evidence: mergeEvidence(existing.evidence, t.evidence ?? {}),
          importance: Math.max(existing.importance, t.importance ?? 0.5)
        })
        updated++
      } else {
        insertKbEntry({
          repoPath,
          topic: t.topic,
          summary: t.summary,
          evidence: t.evidence,
          importance: t.importance,
          tier: 'hot'
        })
        created++
      }
    }
  }
  const digest = (parsed.digest ?? '').trim()
  if (parsed.ok) {
    upsertKbMeta(repoPath, {
      last_summary_at: Date.now(),
      digest
    })
  }
  return { created, updated, digest }
}

/**
 * Merge two evidence blobs: union arrays + de-duplicate. Cap each list at
 * 30 entries so the JSON stored in the row never blows up over time.
 */
export function mergeEvidence(a: KbEvidence, b: KbEvidence): KbEvidence {
  const merge = (x?: string[], y?: string[]): string[] | undefined => {
    if (!x && !y) return undefined
    const set = new Set<string>()
    for (const v of x ?? []) set.add(v)
    for (const v of y ?? []) set.add(v)
    return Array.from(set).slice(0, 30)
  }
  const mergeNum = (x?: number[], y?: number[]): number[] | undefined => {
    if (!x && !y) return undefined
    const set = new Set<number>()
    for (const v of x ?? []) set.add(v)
    for (const v of y ?? []) set.add(v)
    return Array.from(set).slice(0, 30)
  }
  return {
    commits: merge(a.commits, b.commits),
    files: merge(a.files, b.files),
    prompt_ids: mergeNum(a.prompt_ids, b.prompt_ids)
  }
}

/**
 * Helper: build the "existingHotTopics" slice of a SummarySnapshot from the
 * current KB. Pure-ish (reads DB).
 */
export function loadHotTopicSummaries(repoPath: string): {
  topic: string
  summary: string
}[] {
  return listKbEntries(repoPath, { tier: 'hot', limit: 20 }).map((e) => ({
    topic: e.topic,
    summary: e.summary
  }))
}

// Re-exporting for callers that need entry rows alongside the helpers.
export { rowToEntry, type KbEntry }
