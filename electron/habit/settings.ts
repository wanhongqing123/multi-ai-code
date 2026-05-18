import { promises as fs } from 'fs'
import { join } from 'path'
import { rootDir } from '../store/paths.js'
import { ALL_HABIT_EVENT_KINDS, type HabitEventKind } from './db.js'

export interface HabitSettings {
  /** Master switch. If false, all collection is a no-op. */
  enabled: boolean
  /** Per-kind toggles. Missing entries default to true when master is on. */
  kinds: Partial<Record<HabitEventKind, boolean>>
  /** Days to retain raw events. */
  retentionDays: number
  /** When the first-run notice has been shown (epoch ms). 0 = never. */
  firstRunNoticeShownAt: number
  /** Last time the aggregator ran a full pass (epoch ms). 0 = never. */
  lastAggregatedAt: number
}

export const DEFAULT_HABIT_SETTINGS: HabitSettings = {
  enabled: true,
  kinds: Object.fromEntries(ALL_HABIT_EVENT_KINDS.map((k) => [k, true])) as Record<
    HabitEventKind,
    boolean
  >,
  retentionDays: 90,
  firstRunNoticeShownAt: 0,
  lastAggregatedAt: 0
}

export const ALLOWED_RETENTION_DAYS = [30, 60, 90, 180] as const

export function habitSettingsPath(): string {
  return join(rootDir(), 'habit-settings.json')
}

let cache: HabitSettings | null = null

export function clearHabitSettingsCache(): void {
  cache = null
}

export async function loadHabitSettings(): Promise<HabitSettings> {
  if (cache) return cache
  let raw: string
  try {
    raw = await fs.readFile(habitSettingsPath(), 'utf8')
  } catch {
    cache = { ...DEFAULT_HABIT_SETTINGS, kinds: { ...DEFAULT_HABIT_SETTINGS.kinds } }
    return cache
  }
  cache = mergeWithDefaults(safeParse(raw))
  return cache
}

export async function saveHabitSettings(next: HabitSettings): Promise<void> {
  const merged = mergeWithDefaults(next)
  await fs.mkdir(rootDir(), { recursive: true })
  await fs.writeFile(habitSettingsPath(), JSON.stringify(merged, null, 2), 'utf8')
  cache = merged
}

export async function updateHabitSettings(
  patch: Partial<HabitSettings>
): Promise<HabitSettings> {
  const current = await loadHabitSettings()
  const merged = mergeWithDefaults({
    ...current,
    ...patch,
    kinds: { ...current.kinds, ...(patch.kinds ?? {}) }
  })
  await saveHabitSettings(merged)
  return merged
}

export function isKindEnabled(settings: HabitSettings, kind: HabitEventKind): boolean {
  if (!settings.enabled) return false
  const explicit = settings.kinds[kind]
  return explicit !== false
}

function safeParse(raw: string): Partial<HabitSettings> {
  try {
    const parsed = JSON.parse(raw)
    if (parsed && typeof parsed === 'object') return parsed as Partial<HabitSettings>
  } catch {
    /* corrupted file — fall through to defaults */
  }
  return {}
}

export function mergeWithDefaults(input: Partial<HabitSettings>): HabitSettings {
  const kinds: Record<HabitEventKind, boolean> = {
    ...(DEFAULT_HABIT_SETTINGS.kinds as Record<HabitEventKind, boolean>)
  }
  if (input.kinds) {
    for (const k of ALL_HABIT_EVENT_KINDS) {
      const v = input.kinds[k]
      if (typeof v === 'boolean') kinds[k] = v
    }
  }
  const retention = (ALLOWED_RETENTION_DAYS as readonly number[]).includes(
    input.retentionDays as number
  )
    ? (input.retentionDays as number)
    : DEFAULT_HABIT_SETTINGS.retentionDays
  return {
    enabled: typeof input.enabled === 'boolean' ? input.enabled : DEFAULT_HABIT_SETTINGS.enabled,
    kinds,
    retentionDays: retention,
    firstRunNoticeShownAt:
      typeof input.firstRunNoticeShownAt === 'number' ? input.firstRunNoticeShownAt : 0,
    lastAggregatedAt:
      typeof input.lastAggregatedAt === 'number' ? input.lastAggregatedAt : 0
  }
}
