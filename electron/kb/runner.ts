/**
 * KB runner: glues the scheduler decision, data-source readers, CLI
 * subprocess call, parsing, and tier compaction together. Driven from
 * main.ts via a 60-second polling tick.
 *
 * All AI subprocess calls go through the shared `cliQueue` so they
 * serialize with habit-skill generation.
 */

import { promises as fs } from 'fs'
import { basename, join } from 'path'
import {
  CLI_TIMEOUT_MS,
  runCliGeneration,
  type SkillTemplate
} from '../habit/generator.js'
import { enqueueCliJob } from '../util/cliQueue.js'
import { listProjects } from '../store/db.js'
import { projectDir as projectDirFn } from '../store/paths.js'
import {
  countKbEntries,
  getKbMeta,
  upsertKbMeta,
  type KbTier
} from './db.js'
import {
  buildMergePrompt,
  parseMergeResponse,
  runCompaction,
  type MergeFn
} from './compactor.js'
import {
  KB_SCHEDULER_DEFAULTS,
  decideKbRun,
  type KbSchedulerSignals,
  type KbSchedulerThresholds,
  type KbSkipReason
} from './scheduler.js'
import {
  applySummaryToKb,
  buildSummaryPrompt,
  loadHotTopicSummaries,
  parseSummaryResponse,
  type SummarySnapshot
} from './summarizer.js'
import {
  extractHabitText,
  gitRecentCommits,
  recentlyChangedFiles,
  userPromptsSince
} from './sources.js'

interface RawAiSettings {
  ai_cli?: 'claude' | 'codex'
  command?: string
  args?: string[]
  env?: Record<string, string>
}

interface RunnerProjectState {
  projectId: string
  repoPath: string
  /** Cached AI settings — refreshed when getRunnerSignals is called. */
  aiSettings: RawAiSettings | null
  lastAiActivityAt: number
  lastUserPromptAt: number
  pendingSignalCount: number
  mainSessionRunning: boolean
}

const projectState = new Map<string, RunnerProjectState>()
let pollTimer: NodeJS.Timeout | null = null

/** External signal: main-session got new data from the CLI. */
export function noteAiActivity(repoPath: string): void {
  const s = ensureProjectState(repoPath)
  s.lastAiActivityAt = Date.now()
}

/** External signal: user submitted a prompt to the main session. */
export function noteUserPrompt(repoPath: string): void {
  const s = ensureProjectState(repoPath)
  s.lastUserPromptAt = Date.now()
  s.pendingSignalCount += 1
}

/** External signal: main session started / stopped. */
export function setMainSessionRunning(repoPath: string, running: boolean): void {
  const s = ensureProjectState(repoPath)
  s.mainSessionRunning = running
}

/** Cache AI settings discovered by the main process (e.g., on spawn). */
export function setProjectAiSettings(
  projectId: string,
  repoPath: string,
  ai: RawAiSettings | null
): void {
  const s = ensureProjectState(repoPath)
  s.projectId = projectId
  s.aiSettings = ai
}

function ensureProjectState(repoPath: string): RunnerProjectState {
  let s = projectState.get(repoPath)
  if (!s) {
    s = {
      projectId: '',
      repoPath,
      aiSettings: null,
      lastAiActivityAt: 0,
      lastUserPromptAt: 0,
      pendingSignalCount: 0,
      mainSessionRunning: false
    }
    projectState.set(repoPath, s)
  }
  return s
}

/**
 * Build the snapshot the summarizer needs. Reads from the data sources;
 * `sinceTs` controls which user prompts get pulled (set to lastSummaryAt).
 */
async function buildSnapshot(
  repoPath: string,
  sinceTs: number
): Promise<SummarySnapshot> {
  const repoName = basename(repoPath) || repoPath
  const [recentCommits, recentFiles] = await Promise.all([
    gitRecentCommits(repoPath, 50),
    recentlyChangedFiles(repoPath, 30, 30)
  ])
  const prompts = userPromptsSince(sinceTs, repoPath)
    .slice(-20)
    .map(extractHabitText)
    .filter((t) => t && t.length > 0)
  const existingDigest = getKbMeta(repoPath).digest
  return {
    repoPath,
    repoName,
    recentCommits,
    recentFiles,
    recentPrompts: prompts,
    existingHotTopics: loadHotTopicSummaries(repoPath),
    existingDigest
  }
}

export interface RunOnceResult {
  ran: boolean
  reason: 'idle-window' | 'signal-threshold' | 'long-overdue' | KbSkipReason | 'no-state' | 'empty-snapshot' | 'cli-failed'
  topicsCreated?: number
  topicsUpdated?: number
  digestLen?: number
  compaction?: { merged: number; demotedFromHot: number; demotedFromWarm: number }
  error?: string
}

/**
 * Build a CLI-backed `MergeFn` for the compactor that goes through the
 * shared queue. Falls back to null on any spawn / parse failure so the
 * compactor's local-merge fallback kicks in.
 */
function buildAiMergeFn(settings: RawAiSettings): MergeFn {
  return async (entries, targetTier) => {
    if (entries.length === 0) return null
    try {
      const prompt = buildMergePrompt(entries, targetTier)
      const res = await enqueueCliJob('kb-compact', () =>
        runCliGeneration(prompt, settings, { timeoutMs: CLI_TIMEOUT_MS })
      )
      if (!res.ok || !res.template) return null
      // The compact prompt asks for {topic, summary} — but parseGenerationResponse
      // expects {title, steps|body}. We use the dedicated merge parser on the
      // raw template body so we get the right shape regardless.
      return parseMergeResponse(
        (res.template.body ?? res.template.title ?? '').toString()
      )
    } catch {
      return null
    }
  }
}

/**
 * One-shot summary + compaction pass for the given repo. Returns a verdict
 * suitable for IPC. Used both by the polling tick and by the "Run now"
 * button in the UI.
 */
export async function runKbSummaryOnce(repoPath: string): Promise<RunOnceResult> {
  const s = projectState.get(repoPath)
  if (!s) return { ran: false, reason: 'no-state' }
  const settings = s.aiSettings ?? (await fetchSettingsForProject(s.projectId))
  if (!settings) return { ran: false, reason: 'no-cli-config' }
  const meta = getKbMeta(repoPath)
  const snap = await buildSnapshot(repoPath, meta.last_summary_at)

  // Nothing to summarize at all → don't burn an LLM call.
  if (
    snap.recentCommits.length === 0 &&
    snap.recentFiles.length === 0 &&
    snap.recentPrompts.length === 0 &&
    snap.existingHotTopics.length === 0
  ) {
    return { ran: false, reason: 'empty-snapshot' }
  }

  const prompt = buildSummaryPrompt(snap)
  let parsed
  try {
    const res = await enqueueCliJob('kb-summary', () =>
      runCliGeneration(prompt, settings, { timeoutMs: CLI_TIMEOUT_MS })
    )
    if (!res.ok || !res.template) {
      return { ran: false, reason: 'cli-failed', error: res.error }
    }
    parsed = parseSummaryResponse(
      (res.template.body ?? res.template.title ?? '').toString()
    )
  } catch (err) {
    return { ran: false, reason: 'cli-failed', error: (err as Error).message }
  }
  if (!parsed.ok) {
    return { ran: false, reason: 'cli-failed', error: parsed.error }
  }

  const apply = applySummaryToKb(repoPath, parsed)
  // Reset the pending counter on success.
  s.pendingSignalCount = 0

  // Try compaction in the same pass — it's cheap when no tier is over budget.
  let compactionResult: RunOnceResult['compaction']
  try {
    compactionResult = await runCompaction(repoPath, buildAiMergeFn(settings))
    upsertKbMeta(repoPath, { last_compaction_at: Date.now() })
  } catch {
    /* compaction is best-effort */
  }

  return {
    ran: true,
    reason: 'signal-threshold',
    topicsCreated: apply.created,
    topicsUpdated: apply.updated,
    digestLen: apply.digest.length,
    compaction: compactionResult
  }
}

async function fetchSettingsForProject(
  projectId: string
): Promise<RawAiSettings | null> {
  if (!projectId) return null
  try {
    const raw = await fs.readFile(
      join(projectDirFn(projectId), 'project.json'),
      'utf8'
    )
    const meta = JSON.parse(raw) as { ai_settings?: RawAiSettings }
    return meta.ai_settings ?? null
  } catch {
    return null
  }
}

/**
 * Snapshot of a project's scheduler signals — exposed to the UI so the
 * settings panel can show "next run in N min" and the reason for the
 * current skip verdict.
 */
export function projectSchedulerSignals(repoPath: string): KbSchedulerSignals {
  const s = ensureProjectState(repoPath)
  const meta = getKbMeta(repoPath)
  return {
    lastSummaryAt: meta.last_summary_at,
    lastAiActivityAt: s.lastAiActivityAt,
    lastUserPromptAt: s.lastUserPromptAt,
    pendingSignalCount: s.pendingSignalCount,
    mainSessionRunning: s.mainSessionRunning,
    cliConfigured: !!s.aiSettings
  }
}

const inFlight = new Set<string>()

async function tick(thresholds: KbSchedulerThresholds): Promise<void> {
  // Iterate over known projects with state. We don't poll for new projects
  // here — `setMainSessionRunning` adds them on session spawn.
  for (const repoPath of Array.from(projectState.keys())) {
    if (inFlight.has(repoPath)) continue
    const signals = projectSchedulerSignals(repoPath)
    const verdict = decideKbRun(signals, Date.now(), thresholds)
    if (!verdict.run) continue
    inFlight.add(repoPath)
    void runKbSummaryOnce(repoPath)
      .catch(() => undefined)
      .finally(() => inFlight.delete(repoPath))
  }
}

export function startKbScheduler(
  thresholds: KbSchedulerThresholds = KB_SCHEDULER_DEFAULTS,
  intervalMs = 60_000
): void {
  if (pollTimer) return
  pollTimer = setInterval(() => void tick(thresholds), intervalMs)
}

export function stopKbScheduler(): void {
  if (pollTimer) {
    clearInterval(pollTimer)
    pollTimer = null
  }
}

/**
 * Aggregate stats used by the UI's "总览" tab.
 */
export interface KbStats {
  total: number
  byTier: Record<KbTier, number>
  approxBytes: number
  lastSummaryAt: number
  lastCompactionAt: number
}

export function kbStats(repoPath: string): KbStats {
  const meta = getKbMeta(repoPath)
  const total = countKbEntries(repoPath)
  const byTier = {
    hot: countKbEntries(repoPath, 'hot'),
    warm: countKbEntries(repoPath, 'warm'),
    cold: countKbEntries(repoPath, 'cold'),
    pinned: countKbEntries(repoPath, 'pinned')
  }
  return {
    total,
    byTier,
    approxBytes: 0, // recomputed in the UI from the entry list
    lastSummaryAt: meta.last_summary_at,
    lastCompactionAt: meta.last_compaction_at
  }
}

/** Hint for callers in test harness setup. */
export function _resetKbRunnerForTests(): void {
  projectState.clear()
  inFlight.clear()
}

/** Silence unused import warning when `listProjects` ends up unreferenced. */
void listProjects
