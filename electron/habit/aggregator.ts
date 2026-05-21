import type { HabitEventKind, HabitEventRow } from './db.js'

export interface PreparedEvent {
  id: number
  ts: number
  kind: HabitEventKind
  projectId: string | null
  text: string
  /** Normalized tokens for shingling — path/digit noise stripped. */
  normalizedTokens: string[]
  /** Set of length-K shingles for Jaccard. */
  shingles: Set<string>
}

export interface AggregatedCluster {
  /** Stable cluster id within an aggregation run. */
  id: string
  /** Single kind: clustering is per-kind. */
  kind: HabitEventKind
  /** Event ids in this cluster, ts-asc order. */
  sourceEventIds: number[]
  size: number
  /** Up to N representative sample texts. */
  representativeSamples: string[]
  /** Distinct project_id count in this cluster (null projects collapse to a single bucket). */
  projectCount: number
  /** Whether this cluster spans multiple distinct projects. */
  crossProject: boolean
  /** Earliest and latest ts in cluster. */
  firstTs: number
  lastTs: number
  /** Final score = size * recency_decay (see RECENCY_HALF_LIFE_DAYS). */
  score: number
}

export interface AggregateOptions {
  /** Now used for recency decay. Defaults to Date.now(). Pure-function override for tests. */
  now?: number
  /** Minimum cluster size to be kept. Default 3. */
  minClusterSize?: number
  /** Top-K clusters across all kinds. Default 10. */
  topK?: number
  /** Number of representative samples per cluster. Default 5. */
  representativeCount?: number
  /** Shingle length. Default 3 tokens per shingle. */
  shingleK?: number
  /** Jaccard similarity threshold to merge two events. Default 0.4. */
  jaccardThreshold?: number
}

const DEFAULTS = {
  minClusterSize: 3,
  topK: 10,
  representativeCount: 5,
  shingleK: 3,
  jaccardThreshold: 0.4
}

/** Half-life for recency decay in days. */
export const RECENCY_HALF_LIFE_DAYS = 14

/**
 * Strips paths, large numbers, hashes, and other ephemeral noise so the
 * underlying habit (e.g. "look at file X") clusters across different X.
 */
export function normalizeText(text: string): string[] {
  const lowered = text.toLowerCase()
  // Replace path-like substrings with a placeholder.
  let stripped = lowered.replace(/[a-z0-9_./\\-]*[\\/][a-z0-9_./\\-]+/g, ' <path> ')
  // Replace stand-alone hex blobs, long digit runs, line refs (L42, line 42), commit hashes.
  // Range first — otherwise the "lines N" pattern eats N out of "lines N-M".
  stripped = stripped.replace(/\blines?\s+\d+-\d+\b/g, ' <range> ')
  stripped = stripped.replace(/\b\d+-\d+\b/g, ' <range> ')
  stripped = stripped.replace(/\bl\d+\b/g, ' <line> ')
  stripped = stripped.replace(/\blines?\s+\d+\b/g, ' <line> ')
  stripped = stripped.replace(/\b[0-9a-f]{7,}\b/g, ' <hash> ')
  stripped = stripped.replace(/\b\d{2,}\b/g, ' <num> ')
  // Split on whitespace and punctuation, keeping CJK chars intact.
  // Do NOT split on `<>` — the normalizer uses placeholders like `<line>`
  // that must stay as single tokens.
  const tokens = stripped
    .split(/[\s,;.!?:()[\]{}"'`~@#$%^&*+=|\\]+/u)
    .map((t) => t.trim())
    .filter((t) => t.length > 0)
  return tokens
}

export function buildShingles(tokens: string[], k: number): Set<string> {
  const out = new Set<string>()
  if (tokens.length === 0) return out
  if (tokens.length < k) {
    out.add(tokens.join('␟'))
    return out
  }
  for (let i = 0; i + k <= tokens.length; i++) {
    out.add(tokens.slice(i, i + k).join('␟'))
  }
  return out
}

export function jaccard(a: Set<string>, b: Set<string>): number {
  if (a.size === 0 && b.size === 0) return 0
  let inter = 0
  const [smaller, larger] = a.size <= b.size ? [a, b] : [b, a]
  for (const x of smaller) if (larger.has(x)) inter++
  const union = a.size + b.size - inter
  return union === 0 ? 0 : inter / union
}

/**
 * Reads payload text out of a HabitEventRow. Tolerates malformed payloads
 * (empty / non-JSON) and returns '' so they get filtered out downstream.
 */
export function eventText(row: HabitEventRow): string {
  try {
    const parsed = JSON.parse(row.payload) as { text?: string }
    return typeof parsed.text === 'string' ? parsed.text : ''
  } catch {
    return ''
  }
}

function eventPayload(row: HabitEventRow): Record<string, unknown> {
  try {
    const parsed = JSON.parse(row.payload) as Record<string, unknown>
    return parsed && typeof parsed === 'object' ? parsed : {}
  } catch {
    return {}
  }
}

function extractUrlBits(raw: string): { origin: string; path: string } | null {
  const match = raw.match(/https?:\/\/[^\s)]+/i)
  const candidate = match?.[0] ?? raw
  try {
    const parsed = new URL(candidate)
    return {
      origin: parsed.host,
      path: parsed.pathname || '/'
    }
  } catch {
    return null
  }
}

function clusteringText(row: HabitEventRow): string {
  const payload = eventPayload(row)
  const rawText = eventText(row)

  if (row.kind === 'site_visit') {
    const fromPayload =
      typeof payload.origin === 'string' && typeof payload.path === 'string'
        ? { origin: payload.origin, path: payload.path }
        : typeof payload.url === 'string'
          ? extractUrlBits(payload.url)
          : extractUrlBits(rawText)
    if (fromPayload) {
      return `visit ${fromPayload.origin} ${fromPayload.path}`
    }
  }

  if (row.kind === 'site_click' || row.kind === 'site_input_hint') {
    const urlBits =
      typeof payload.origin === 'string' && typeof payload.path === 'string'
        ? { origin: payload.origin, path: payload.path }
        : typeof payload.url === 'string'
          ? extractUrlBits(payload.url)
          : extractUrlBits(rawText)
    const elementHint =
      typeof payload.elementHint === 'string'
        ? payload.elementHint
        : rawText
    return [
      row.kind === 'site_click' ? 'click' : 'input',
      urlBits?.origin,
      urlBits?.path,
      typeof payload.role === 'string' ? payload.role : undefined,
      typeof payload.inputType === 'string' ? payload.inputType : undefined,
      elementHint
    ]
      .filter((part): part is string => typeof part === 'string' && part.trim().length > 0)
      .join(' ')
  }

  if (row.kind === 'panel_open') {
    const panelId =
      typeof payload.panelId === 'string'
        ? payload.panelId
        : rawText.toLowerCase().includes('build')
          ? 'build'
          : rawText
    return `panel ${panelId}`
  }

  if (row.kind === 'action_triggered') {
    const actionId = typeof payload.actionId === 'string' ? payload.actionId : rawText
    return `action ${actionId}`
  }

  return rawText
}

export function prepareEvent(row: HabitEventRow, shingleK: number): PreparedEvent {
  const text = eventText(row)
  const normalizedTokens = normalizeText(clusteringText(row))
  const shingles = buildShingles(normalizedTokens, shingleK)
  return {
    id: row.id,
    ts: row.ts,
    kind: row.kind,
    projectId: row.project_id,
    text,
    normalizedTokens,
    shingles
  }
}

/**
 * Union-find based clustering. Each event becomes a node; we connect
 * pairs whose Jaccard ≥ threshold and emit connected components. O(n²)
 * pairwise — fine for the volumes we expect (≤ 30 days × moderate use).
 */
function unionFindCluster(
  events: PreparedEvent[],
  threshold: number
): number[][] {
  const parent: number[] = events.map((_, i) => i)
  const find = (i: number): number => {
    let r = i
    while (parent[r] !== r) r = parent[r]
    let cur = i
    while (parent[cur] !== r) {
      const next = parent[cur]
      parent[cur] = r
      cur = next
    }
    return r
  }
  const union = (a: number, b: number) => {
    const ra = find(a)
    const rb = find(b)
    if (ra !== rb) parent[ra] = rb
  }
  for (let i = 0; i < events.length; i++) {
    for (let j = i + 1; j < events.length; j++) {
      const sim = jaccard(events[i].shingles, events[j].shingles)
      if (sim >= threshold) union(i, j)
    }
  }
  const groups = new Map<number, number[]>()
  for (let i = 0; i < events.length; i++) {
    const r = find(i)
    const arr = groups.get(r) ?? []
    arr.push(i)
    groups.set(r, arr)
  }
  return Array.from(groups.values())
}

export function recencyDecay(
  ts: number,
  now: number,
  halfLifeDays = RECENCY_HALF_LIFE_DAYS
): number {
  const dayMs = 24 * 60 * 60 * 1000
  const ageDays = Math.max(0, (now - ts) / dayMs)
  return Math.exp(-(ageDays * Math.LN2) / halfLifeDays)
}

/**
 * Picks the N most "central" samples in a cluster: the events whose
 * average Jaccard against the rest of the cluster is highest. Falls back to
 * most-recent when the cluster is too small to compare.
 */
function pickRepresentatives(events: PreparedEvent[], count: number): string[] {
  if (events.length <= count) {
    return events
      .slice()
      .sort((a, b) => b.ts - a.ts)
      .map((e) => e.text)
  }
  const scored = events.map((e, idx) => {
    let sum = 0
    for (let j = 0; j < events.length; j++) {
      if (j === idx) continue
      sum += jaccard(e.shingles, events[j].shingles)
    }
    return { e, score: sum / (events.length - 1) }
  })
  scored.sort((a, b) => b.score - a.score)
  return scored.slice(0, count).map((s) => s.e.text)
}

export function aggregateHabitEvents(
  rows: HabitEventRow[],
  opts: AggregateOptions = {}
): AggregatedCluster[] {
  const cfg = { ...DEFAULTS, ...opts }
  const now = opts.now ?? Date.now()
  // Group by kind first — we never merge across kinds.
  const byKind = new Map<HabitEventKind, PreparedEvent[]>()
  for (const row of rows) {
    const prepared = prepareEvent(row, cfg.shingleK)
    if (prepared.normalizedTokens.length === 0) continue
    const arr = byKind.get(row.kind) ?? []
    arr.push(prepared)
    byKind.set(row.kind, arr)
  }

  const clusters: AggregatedCluster[] = []
  for (const [kind, events] of byKind) {
    const groups = unionFindCluster(events, cfg.jaccardThreshold)
    for (const indices of groups) {
      if (indices.length < cfg.minClusterSize) continue
      const members = indices.map((i) => events[i])
      members.sort((a, b) => a.ts - b.ts)
      const projectIds = new Set(members.map((m) => m.projectId ?? '__none__'))
      const lastTs = Math.max(...members.map((m) => m.ts))
      const firstTs = Math.min(...members.map((m) => m.ts))
      const decay = recencyDecay(lastTs, now)
      const score = members.length * decay
      clusters.push({
        id: `${kind}:${firstTs}:${members[0].id}`,
        kind,
        sourceEventIds: members.map((m) => m.id),
        size: members.length,
        representativeSamples: pickRepresentatives(members, cfg.representativeCount),
        projectCount: projectIds.size,
        crossProject: projectIds.size > 1,
        firstTs,
        lastTs,
        score
      })
    }
  }

  clusters.sort((a, b) => b.score - a.score)
  return clusters.slice(0, cfg.topK)
}
