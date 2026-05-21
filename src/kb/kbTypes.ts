/**
 * Renderer-side mirror of the KB types defined in electron/kb/db.ts.
 * Kept in sync by hand because the renderer cannot import Electron-only
 * modules (better-sqlite3) directly.
 */

export type KbTier = 'hot' | 'warm' | 'cold' | 'pinned'

export const ALL_KB_TIERS: KbTier[] = ['hot', 'warm', 'cold', 'pinned']

export interface KbEvidence {
  commits?: string[]
  files?: string[]
  prompt_ids?: number[]
}

export interface KbEntry {
  id: number
  repoPath: string
  createdAt: number
  updatedAt: number
  topic: string
  summary: string
  evidence: KbEvidence
  importance: number
  tier: KbTier
  accessCount: number
  lastAccessedAt: number | null
}

export interface KbSearchResult {
  entry: KbEntry
  score: number
}

export const KB_TIER_LABELS: Record<KbTier, string> = {
  hot: '热（最近）',
  warm: '温（主题段）',
  cold: '冷（长期）',
  pinned: '⭐ 收藏'
}

/** Mirror of the importance score formula used by the main process. */
export function computeKbImportance(
  ageDays: number,
  accessCount: number,
  pinned: boolean
): number {
  if (pinned) return 999
  const recency = Math.exp(-Math.max(0, ageDays) / 14)
  const freq = Math.log(1 + Math.max(0, accessCount))
  return recency * (freq + 0.001)
}
