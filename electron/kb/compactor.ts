/**
 * Tier compaction: when the KB's hot/warm tiers exceed their caps, this
 * module demotes the lowest-score entries by collapsing several of them
 * into a single coarser entry one tier down.
 *
 * Importance scoring + the decision logic are pure functions; the actual
 * write path lives in `runCompaction` below.
 */

import {
  deleteKbEntry,
  insertKbEntry,
  listKbEntries,
  type KbEntry,
  type KbTier
} from './db.js'
import { mergeEvidence } from './summarizer.js'

export interface KbTierBudgets {
  hot: number
  warm: number
  cold: number
  /** Hard cap on total bytes across all entries' summaries (rough estimate). */
  totalBytes: number
}

export const KB_TIER_DEFAULTS: KbTierBudgets = {
  hot: 30,
  warm: 30,
  cold: 5,
  totalBytes: 500 * 1024
}

/** Half-life for the recency component of the importance score (days). */
export const KB_RECENCY_HALF_LIFE_DAYS = 14

/**
 * importance score = recency_decay × log(1 + access_count); pinned entries
 * dominate with a sentinel value so they never get demoted.
 */
export function importanceOf(entry: KbEntry, now: number): number {
  if (entry.tier === 'pinned') return Number.POSITIVE_INFINITY
  const dayMs = 24 * 60 * 60 * 1000
  const ageDays = Math.max(0, (now - entry.updatedAt) / dayMs)
  const recency = Math.exp(-ageDays / KB_RECENCY_HALF_LIFE_DAYS)
  const freq = Math.log(1 + Math.max(0, entry.accessCount))
  // The persisted `importance` field acts as a soft boost (e.g., AI-estimated
  // structural importance). Use it as a multiplier with a 0.5 floor so a
  // zero-importance entry isn't completely buried.
  const boost = Math.max(0.5, Math.min(2, entry.importance + 0.5))
  return recency * (freq + 0.001) * boost
}

export interface CompactionPlan {
  /** Entries that should move out of hot (lowest-scored at the bottom). */
  demoteFromHot: KbEntry[]
  /** Entries that should move out of warm. */
  demoteFromWarm: KbEntry[]
  /** True when the total stored-text budget has been exceeded. */
  totalBytesOver: boolean
  /** Bytes the KB currently occupies (approx, summary text only). */
  approxBytes: number
}

/**
 * Pure planner — decides what to demote without writing anything. Tested
 * standalone.
 */
export function planCompaction(
  entries: KbEntry[],
  now: number,
  budgets: KbTierBudgets = KB_TIER_DEFAULTS
): CompactionPlan {
  const hot = entries
    .filter((e) => e.tier === 'hot')
    .map((e) => ({ e, s: importanceOf(e, now) }))
    .sort((a, b) => a.s - b.s)
  const warm = entries
    .filter((e) => e.tier === 'warm')
    .map((e) => ({ e, s: importanceOf(e, now) }))
    .sort((a, b) => a.s - b.s)

  const demoteFromHot = hot.length > budgets.hot
    ? hot.slice(0, hot.length - budgets.hot).map((x) => x.e)
    : []
  const demoteFromWarm = warm.length > budgets.warm
    ? warm.slice(0, warm.length - budgets.warm).map((x) => x.e)
    : []
  const approxBytes = entries.reduce(
    (sum, e) => sum + (e.summary.length + e.topic.length) * 2, // UTF-16 estimate
    0
  )
  return {
    demoteFromHot,
    demoteFromWarm,
    totalBytesOver: approxBytes > budgets.totalBytes,
    approxBytes
  }
}

/**
 * The CLI-backed merge function. Given N entries from the same tier, it
 * asks the AI to fold them into a single coarser entry at the next tier.
 * Returns the new (merged) entry data, or null on failure (caller falls
 * back to a local concat strategy so the demotion still happens).
 */
export type MergeFn = (
  entries: KbEntry[],
  targetTier: 'warm' | 'cold'
) => Promise<{ topic: string; summary: string } | null>

/**
 * Builds the prompt used to ask the CLI to merge several entries. Pure so
 * we can test the wording / privacy invariants without spawning anything.
 */
export function buildMergePrompt(
  entries: KbEntry[],
  targetTier: 'warm' | 'cold'
): string {
  const lines: string[] = []
  lines.push(
    `请把下面 ${entries.length} 条 "${entries[0]?.repoPath ? '同一项目' : ''}" 的知识库条目合并成一个更粗粒度的 ${targetTier} 段：`
  )
  lines.push('')
  for (let i = 0; i < entries.length; i++) {
    lines.push(`[条目 ${i + 1}] ${entries[i].topic}`)
    lines.push(entries[i].summary)
    lines.push('')
  }
  lines.push('[要求]')
  lines.push('返回严格 JSON：{"topic":"...","summary":"..."}')
  lines.push(
    '- topic: 一个能概括这一组的主题名（不超过 20 字）'
  )
  lines.push(
    '- summary: 把上面所有条目的关键信息合并成 3-5 句话，删掉重复，保留事实'
  )
  return lines.join('\n')
}

/**
 * Pure parser for the merge response. Same tolerance as the summarizer
 * parser (fence-stripping, surrounding chatter).
 */
export function parseMergeResponse(
  raw: string
): { topic: string; summary: string } | null {
  if (!raw || typeof raw !== 'string') return null
  let candidate = raw.trim()
  const fenced = candidate.match(/```(?:json)?\s*([\s\S]*?)\s*```/i)
  if (fenced && fenced[1]) candidate = fenced[1].trim()
  if (!candidate.startsWith('{')) {
    const first = candidate.indexOf('{')
    const last = candidate.lastIndexOf('}')
    if (first >= 0 && last > first) candidate = candidate.slice(first, last + 1)
  }
  try {
    const parsed = JSON.parse(candidate) as Record<string, unknown>
    const topic = typeof parsed.topic === 'string' ? parsed.topic.trim() : ''
    const summary = typeof parsed.summary === 'string' ? parsed.summary.trim() : ''
    if (!topic || !summary) return null
    return { topic, summary }
  } catch {
    return null
  }
}

/**
 * Local fallback merger — used when the CLI call fails so the demotion
 * still happens (otherwise the KB would never shrink). Quality is lower
 * than AI-merged, but the entries become discoverable in their new tier.
 */
export function localMerge(
  entries: KbEntry[],
  targetTier: KbTier
): { topic: string; summary: string } {
  const topic = entries.length === 1
    ? entries[0].topic
    : `合并：${entries.slice(0, 3).map((e) => e.topic).join(' / ')}${entries.length > 3 ? ' 等' : ''}`
  const summary = entries.map((e) => `• ${e.topic}: ${e.summary}`).join('\n')
  // Cap to keep cold-tier summaries from blowing out.
  const cap = targetTier === 'cold' ? 2000 : 1200
  return {
    topic: topic.slice(0, 80),
    summary: summary.length > cap ? summary.slice(0, cap) + '…' : summary
  }
}

/**
 * Run one compaction pass. `merge` is dependency-injected so tests can
 * substitute a mock. The function:
 *   1. Plans demotions (using `planCompaction`)
 *   2. For each demotion group, asks `merge` for a coarser entry
 *   3. Falls back to local merge if AI returns null
 *   4. Writes the new entry and deletes the originals
 *
 * Pinned entries are skipped automatically (importanceOf returns +Inf).
 */
export async function runCompaction(
  repoPath: string,
  merge: MergeFn,
  now: number = Date.now(),
  budgets: KbTierBudgets = KB_TIER_DEFAULTS
): Promise<{ merged: number; demotedFromHot: number; demotedFromWarm: number }> {
  const all = listKbEntries(repoPath)
  const plan = planCompaction(all, now, budgets)
  let merged = 0

  async function demoteGroup(
    group: KbEntry[],
    target: 'warm' | 'cold'
  ): Promise<void> {
    if (group.length === 0) return
    let res: { topic: string; summary: string } | null = null
    try {
      res = await merge(group, target)
    } catch {
      res = null
    }
    const final = res ?? localMerge(group, target)
    const mergedEvidence = group.reduce(
      (acc, e) => mergeEvidence(acc, e.evidence),
      {}
    )
    insertKbEntry({
      repoPath,
      topic: final.topic,
      summary: final.summary,
      evidence: mergedEvidence,
      importance: Math.max(...group.map((g) => g.importance), 0.5),
      tier: target
    })
    for (const e of group) deleteKbEntry(e.id)
    merged++
  }

  // Hot → warm: bundle the lowest-scored 5 into one warm entry per pass.
  const BUNDLE = 5
  for (let i = 0; i < plan.demoteFromHot.length; i += BUNDLE) {
    await demoteGroup(plan.demoteFromHot.slice(i, i + BUNDLE), 'warm')
  }
  // Warm → cold: bundle 5 likewise.
  for (let i = 0; i < plan.demoteFromWarm.length; i += BUNDLE) {
    await demoteGroup(plan.demoteFromWarm.slice(i, i + BUNDLE), 'cold')
  }

  return {
    merged,
    demotedFromHot: plan.demoteFromHot.length,
    demotedFromWarm: plan.demoteFromWarm.length
  }
}
