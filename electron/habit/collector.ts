import { insertHabitEvent, type HabitEventKind, type HabitEventSource } from './db.js'
import { isKindEnabled, loadHabitSettings } from './settings.js'

export interface RecordHabitEventInput {
  kind: HabitEventKind
  text: string
  source?: HabitEventSource
  projectId?: string
  repoPath?: string
  sourceWindow?: string
  extras?: Record<string, unknown>
}

/** Minimum text length we bother to record. Sub-threshold inputs are noise. */
export const MIN_RECORD_TEXT_LENGTH = 8

/**
 * Throttle window for identical text. The same exact text inside this window
 * is recorded only once. Prevents enter-spam and double-clicks from inflating
 * counts. 24h matches the design doc's "≤ 1 per 24h per identical text".
 */
const DEDUPE_WINDOW_MS = 24 * 60 * 60 * 1000

const recentTextStamps = new Map<string, number>()

export function clearRecordHabitDedupeForTests(): void {
  recentTextStamps.clear()
}

export function shouldRecordText(text: string, now: number = Date.now()): boolean {
  const trimmed = text.trim()
  if (trimmed.length < MIN_RECORD_TEXT_LENGTH) return false
  const lastAt = recentTextStamps.get(trimmed)
  if (lastAt !== undefined && now - lastAt < DEDUPE_WINDOW_MS) return false
  // Garbage-collect old entries opportunistically.
  if (recentTextStamps.size > 5000) {
    const cutoff = now - DEDUPE_WINDOW_MS
    for (const [k, v] of recentTextStamps) {
      if (v < cutoff) recentTextStamps.delete(k)
    }
  }
  recentTextStamps.set(trimmed, now)
  return true
}

/**
 * Best-effort, never-throw event recorder. Safe to call from any user-facing
 * code path: if settings disable this kind, or the DB write fails, the call
 * is silently dropped so it never breaks the host action.
 */
export async function recordHabitEvent(input: RecordHabitEventInput): Promise<void> {
  try {
    const settings = await loadHabitSettings()
    if (!isKindEnabled(settings, input.kind)) return
    if (!shouldRecordText(input.text)) return
    const payload = {
      text: input.text.trim(),
      ...input.extras
    }
    insertHabitEvent({
      ts: Date.now(),
      kind: input.kind,
      payload,
      source: input.source ?? 'app_ui',
      projectId: input.projectId,
      repoPath: input.repoPath,
      sourceWindow: input.sourceWindow
    })
  } catch {
    /* swallow — collection must never break the host action */
  }
}
