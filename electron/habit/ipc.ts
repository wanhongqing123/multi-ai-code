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
  clearAllHabitFlows,
  clearAllSkillCandidates,
  countHabitEvents,
  deleteHabitEventsBefore,
  listHabitFlows,
  listRecentHabitEvents,
  listSkillCandidates,
  upsertManagedChromeSession,
  updateHabitFlowStatus,
  updateSkillCandidateStatus,
  type HabitFlowStatus,
  type SkillCandidateStatus
} from './db.js'
import { runAggregationCoalesced } from './scheduler.js'
import { getSkillGenerator } from './generatorRegistry.js'
import type { ManagedChromeManager, ManagedChromeState } from './managedChrome.js'

const IDLE_MANAGED_CHROME_STATE: ManagedChromeState = {
  running: false,
  port: null,
  profileDir: null,
  pid: null,
  lastActiveUrl: null
}

function persistManagedChromeSession(state: ManagedChromeState, now: number, running: boolean): void {
  if (state.port === null || state.profileDir === null) return
  upsertManagedChromeSession({
    port: state.port,
    profileDir: state.profileDir,
    startedAt: now,
    lastActiveAt: now,
    running,
    lastActiveUrl: state.lastActiveUrl
  })
}

export function registerHabitIpc(opts: { managedChromeManager?: ManagedChromeManager } = {}): void {
  const managedChromeManager = opts.managedChromeManager

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

  ipcMain.handle('habit:chrome:get-state', async () => {
    return managedChromeManager?.getState() ?? IDLE_MANAGED_CHROME_STATE
  })

  ipcMain.handle('habit:chrome:start', async () => {
    if (!managedChromeManager) {
      return { ok: false as const, error: 'managed Chrome manager unavailable' }
    }
    try {
      const state = await managedChromeManager.start()
      persistManagedChromeSession(state, Date.now(), true)
      return { ok: true as const, value: state }
    } catch (err) {
      return {
        ok: false as const,
        error: err instanceof Error ? err.message : String(err)
      }
    }
  })

  ipcMain.handle('habit:chrome:stop', async () => {
    if (!managedChromeManager) {
      return { ok: false as const, error: 'managed Chrome manager unavailable' }
    }
    const prev = managedChromeManager.getState()
    try {
      await managedChromeManager.stop()
      persistManagedChromeSession(prev, Date.now(), false)
      return { ok: true as const }
    } catch (err) {
      return {
        ok: false as const,
        error: err instanceof Error ? err.message : String(err)
      }
    }
  })

  ipcMain.handle('habit:chrome:focus', async () => {
    if (!managedChromeManager) {
      return { ok: false as const, error: 'managed Chrome manager unavailable' }
    }
    try {
      await managedChromeManager.focus()
      const state = managedChromeManager.getState()
      persistManagedChromeSession(state, Date.now(), true)
      return { ok: true as const }
    } catch (err) {
      return {
        ok: false as const,
        error: err instanceof Error ? err.message : String(err)
      }
    }
  })

  ipcMain.handle(
    'habit:flows:list',
    async (_e, opts?: { statuses?: HabitFlowStatus[]; limit?: number }) => {
      return listHabitFlows(opts)
    }
  )

  ipcMain.handle(
    'habit:flows:update-status',
    async (_e, { id, status }: { id: number; status: HabitFlowStatus }) => {
      updateHabitFlowStatus(id, status)
      return { ok: true }
    }
  )

  ipcMain.handle('habit:flows:clear', async () => {
    const removed = clearAllHabitFlows()
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
