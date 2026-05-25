import { aggregateHabitEvents, type AggregatedCluster } from './aggregator.js'
import {
  clearAllHabitFlows,
  insertSkillCandidate,
  insertHabitFlow,
  listHabitEventsSince,
  listHabitFlows,
  listSkillCandidates,
  type SkillCandidateRow
} from './db.js'
import { loadHabitSettings, updateHabitSettings } from './settings.js'
import { deleteHabitEventsBefore } from './db.js'
import { generateFlowsFromClusters, type GeneratedFlow } from './flowEngine.js'

/** 24h between full aggregation passes. */
export const AGGREGATION_INTERVAL_MS = 24 * 60 * 60 * 1000
/** Lookback window for aggregation: events within last 30 days. */
export const AGGREGATION_WINDOW_MS = 30 * 24 * 60 * 60 * 1000
/** Delay before the first opportunistic check after app start. */
export const STARTUP_GRACE_MS = 60 * 1000

let pollTimer: NodeJS.Timeout | null = null
let inFlight: Promise<RunOutcome> | null = null

export interface RunOutcome {
  ran: boolean
  reason: 'disabled' | 'too-soon' | 'no-events' | 'completed' | 'no-clusters'
  clustersFound?: number
  flowsGenerated?: number
  candidatesInserted?: number
  skippedExistingClusterIds?: string[]
  startedAt: number
  finishedAt: number
}

function flowSignatureFromGenerated(flow: GeneratedFlow): string {
  return `${flow.kind}|${flow.riskLevel}|${JSON.stringify(flow.payload)}`
}

function flowSignatureFromRow(row: {
  kind: string
  risk_level: string
  payload: string
}): string {
  return `${row.kind}|${row.risk_level}|${row.payload}`
}

function statusForGeneratedFlow(
  flow: GeneratedFlow,
  existingBySignature: Map<string, { status: 'candidate' | 'active' | 'disabled' }>
): 'candidate' | 'active' | 'disabled' {
  const existing = existingBySignature.get(flowSignatureFromGenerated(flow))
  if (existing) return existing.status
  return flow.enabledByDefault ? 'active' : 'candidate'
}

function replaceHabitFlows(flows: GeneratedFlow[]): number {
  const existingBySignature = new Map(
    listHabitFlows({ limit: 500 }).map((row) => [
      flowSignatureFromRow(row),
      { status: row.status }
    ])
  )

  clearAllHabitFlows()

  for (const flow of flows) {
    insertHabitFlow({
      kind: flow.kind,
      title: flow.title,
      summary: flow.summary,
      evidenceCount: flow.evidenceCount,
      riskLevel: flow.riskLevel,
      enabledByDefault: flow.enabledByDefault,
      status: statusForGeneratedFlow(flow, existingBySignature),
      payload: flow.payload
    })
  }

  return flows.length
}

/**
 * Side-effect free planner: given a settings snapshot + clock, decide whether
 * a full aggregation pass should run right now. Tested in isolation.
 */
export function shouldRunAggregation(
  settings: { enabled: boolean; lastAggregatedAt: number },
  now: number,
  intervalMs = AGGREGATION_INTERVAL_MS
): boolean {
  if (!settings.enabled) return false
  if (settings.lastAggregatedAt <= 0) return true
  return now - settings.lastAggregatedAt >= intervalMs
}

/**
 * Decide which clusters from the new aggregation result are genuinely "new"
 * and worth a candidate row. Drops clusters whose source_event_ids fully
 * overlap with an existing candidate's source_event_ids.
 */
export function pickNewClusters(
  clusters: AggregatedCluster[],
  existing: SkillCandidateRow[]
): AggregatedCluster[] {
  const existingIdSets = existing.map((r) => {
    try {
      return new Set(JSON.parse(r.source_event_ids) as number[])
    } catch {
      return new Set<number>()
    }
  })
  return clusters.filter((c) => {
    const cSet = new Set(c.sourceEventIds)
    for (const ex of existingIdSets) {
      if (cSet.size > 0 && [...cSet].every((id) => ex.has(id))) {
        // Fully subsumed by an existing candidate.
        return false
      }
    }
    return true
  })
}

export type GenerateFn = (
  cluster: AggregatedCluster
) => Promise<
  | {
      ok: true
      title: string
      /** Legacy single-prompt body. Optional now — steps is canonical. */
      body?: string
      /** Multi-step recipe. Persisted inside meta JSON for the candidate row. */
      steps?: unknown[]
      /** Short keyword used by the SkillBar autocomplete after the candidate is accepted. */
      trigger?: string
      meta?: unknown
    }
  | { ok: false; error: string }
  /** Skip this cluster without inserting a candidate row. */
  | { skip: true }
>

/**
 * Runs one aggregation pass. Pure-ish: takes a generator function so the
 * test can substitute the real LLM call.
 */
export async function runAggregationOnce(
  generate: GenerateFn,
  now: number = Date.now()
): Promise<RunOutcome> {
  const startedAt = now
  const settings = await loadHabitSettings()
  if (!settings.enabled) {
    return {
      ran: false,
      reason: 'disabled',
      startedAt,
      finishedAt: Date.now()
    }
  }

  // Retention sweep: drop events older than retentionDays before aggregating.
  const retentionCutoff = startedAt - settings.retentionDays * 24 * 60 * 60 * 1000
  try {
    deleteHabitEventsBefore(retentionCutoff)
  } catch {
    /* DB may not be ready in tests; ignore */
  }

  const since = startedAt - AGGREGATION_WINDOW_MS
  let rows: ReturnType<typeof listHabitEventsSince>
  try {
    rows = listHabitEventsSince(since)
  } catch {
    rows = []
  }

  if (rows.length === 0) {
    clearAllHabitFlows()
    await updateHabitSettings({ lastAggregatedAt: startedAt })
    return {
      ran: true,
      reason: 'no-events',
      clustersFound: 0,
      flowsGenerated: 0,
      candidatesInserted: 0,
      startedAt,
      finishedAt: Date.now()
    }
  }

  const clusters = aggregateHabitEvents(rows, { now: startedAt })
  if (clusters.length === 0) {
    clearAllHabitFlows()
    await updateHabitSettings({ lastAggregatedAt: startedAt })
    return {
      ran: true,
      reason: 'no-clusters',
      clustersFound: 0,
      flowsGenerated: 0,
      candidatesInserted: 0,
      startedAt,
      finishedAt: Date.now()
    }
  }

  const flowsGenerated = replaceHabitFlows(generateFlowsFromClusters(clusters))

  const existing = listSkillCandidates({ limit: 500 })
  const fresh = pickNewClusters(clusters, existing)

  let inserted = 0
  for (const cluster of fresh) {
    try {
      const result = await generate(cluster)
      if ('skip' in result && result.skip) {
        continue
      }
      if ('ok' in result && result.ok) {
        // Multi-step recipe and short trigger keyword live inside the meta
        // JSON so the renderer can hydrate them when the user accepts a
        // candidate (no schema migration needed).
        const baseMeta =
          result.meta && typeof result.meta === 'object'
            ? (result.meta as Record<string, unknown>)
            : {}
        const enrichedMeta = {
          ...baseMeta,
          steps: result.steps,
          trigger: result.trigger
        }
        insertSkillCandidate({
          createdAt: Date.now(),
          clusterKind: cluster.kind,
          clusterSize: cluster.size,
          sourceEventIds: cluster.sourceEventIds,
          representativeSamples: cluster.representativeSamples,
          generatedTitle: result.title,
          generatedBody: result.body,
          generatedMeta: enrichedMeta,
          status: 'pending'
        })
      } else if ('ok' in result && !result.ok) {
        insertSkillCandidate({
          createdAt: Date.now(),
          clusterKind: cluster.kind,
          clusterSize: cluster.size,
          sourceEventIds: cluster.sourceEventIds,
          representativeSamples: cluster.representativeSamples,
          status: 'error',
          errorMessage: result.error
        })
      }
      inserted++
    } catch (err) {
      insertSkillCandidate({
        createdAt: Date.now(),
        clusterKind: cluster.kind,
        clusterSize: cluster.size,
        sourceEventIds: cluster.sourceEventIds,
        representativeSamples: cluster.representativeSamples,
        status: 'error',
        errorMessage: err instanceof Error ? err.message : String(err)
      })
      inserted++
    }
  }

  await updateHabitSettings({ lastAggregatedAt: startedAt })

  return {
    ran: true,
    reason: 'completed',
    clustersFound: clusters.length,
    flowsGenerated,
    candidatesInserted: inserted,
    startedAt,
    finishedAt: Date.now()
  }
}

/**
 * Triggers an aggregation pass, but coalesces concurrent calls — if one is
 * already in flight, the new caller gets the same promise.
 */
export function runAggregationCoalesced(
  generate: GenerateFn,
  now: number = Date.now()
): Promise<RunOutcome> {
  if (inFlight) return inFlight
  inFlight = runAggregationOnce(generate, now).finally(() => {
    inFlight = null
  })
  return inFlight
}

/**
 * Background poller that wakes up every hour, checks shouldRunAggregation,
 * and if true triggers a coalesced run.
 */
export function startScheduler(generate: GenerateFn): void {
  if (pollTimer) return
  const POLL_INTERVAL_MS = 60 * 60 * 1000 // 1h
  const tick = async () => {
    try {
      const settings = await loadHabitSettings()
      if (shouldRunAggregation(settings, Date.now())) {
        await runAggregationCoalesced(generate)
      }
    } catch {
      /* poller must not throw */
    }
  }
  pollTimer = setInterval(() => void tick(), POLL_INTERVAL_MS)
  // Grace period after startup before the first attempt.
  setTimeout(() => void tick(), STARTUP_GRACE_MS)
}

export function stopScheduler(): void {
  if (pollTimer) {
    clearInterval(pollTimer)
    pollTimer = null
  }
}
