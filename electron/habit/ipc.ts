import { ipcMain } from 'electron'
import { recordHabitEvent, type RecordHabitEventInput } from './collector.js'
import {
  loadHabitSettings,
  saveHabitSettings,
  updateHabitSettings,
  type HabitSettings
} from './settings.js'
import {
  clearAllHabitEvents,
  clearAllSkillCandidates,
  countHabitEvents,
  deleteHabitEventsBefore,
  listRecentHabitEvents,
  listSkillCandidates,
  updateSkillCandidateStatus,
  type SkillCandidateStatus
} from './db.js'
import { runAggregationCoalesced } from './scheduler.js'
import { getSkillGenerator } from './generatorRegistry.js'

export function registerHabitIpc(): void {
  ipcMain.handle('habit:record', async (_e, input: RecordHabitEventInput) => {
    await recordHabitEvent(input)
    return { ok: true }
  })

  ipcMain.handle('habit:settings:get', async () => {
    return loadHabitSettings()
  })

  ipcMain.handle('habit:settings:save', async (_e, next: HabitSettings) => {
    await saveHabitSettings(next)
    return { ok: true }
  })

  ipcMain.handle('habit:settings:update', async (_e, patch: Partial<HabitSettings>) => {
    return updateHabitSettings(patch)
  })

  ipcMain.handle('habit:events:recent', async (_e, { limit = 100 }: { limit?: number } = {}) => {
    return { events: listRecentHabitEvents(limit), total: countHabitEvents() }
  })

  ipcMain.handle('habit:events:clear', async () => {
    const removed = clearAllHabitEvents()
    return { ok: true, removed }
  })

  ipcMain.handle(
    'habit:events:retention-sweep',
    async (_e, { beforeTs }: { beforeTs: number }) => {
      const removed = deleteHabitEventsBefore(beforeTs)
      return { ok: true, removed }
    }
  )

  ipcMain.handle(
    'habit:candidates:list',
    async (_e, opts?: { statuses?: SkillCandidateStatus[]; limit?: number }) => {
      return listSkillCandidates(opts)
    }
  )

  ipcMain.handle(
    'habit:candidates:update-status',
    async (
      _e,
      {
        id,
        status,
        snoozedUntil,
        errorMessage
      }: {
        id: number
        status: SkillCandidateStatus
        snoozedUntil?: number | null
        errorMessage?: string | null
      }
    ) => {
      updateSkillCandidateStatus(id, status, { snoozedUntil, errorMessage })
      return { ok: true }
    }
  )

  ipcMain.handle('habit:candidates:clear', async () => {
    const removed = clearAllSkillCandidates()
    return { ok: true, removed }
  })

  ipcMain.handle('habit:run-now', async () => {
    try {
      const outcome = await runAggregationCoalesced(getSkillGenerator())
      return { ok: true as const, outcome }
    } catch (err) {
      return {
        ok: false as const,
        error: err instanceof Error ? err.message : String(err)
      }
    }
  })
}
