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
  updateHabitFlowStatus,
  updateSkillCandidateStatus,
  type HabitFlowStatus,
  type SkillCandidateStatus
} from './db.js'
import { runAggregationCoalesced } from './scheduler.js'
import { getSkillGenerator } from './generatorRegistry.js'
import {
  getScreenSamplerStatus,
  refreshScreenSamplerLiveState,
  setScreenSamplerPaused,
  toggleScreenSamplerPause
} from './screenSamplerService.js'
import {
  createSkill,
  deleteSkill,
  getSkill,
  listSkills,
  touchSkillLastUsed,
  updateSkill,
  type CreateSkillInput,
  type SkillStep,
  type UpdateSkillInput
} from './skills.js'

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
    const next = await updateHabitSettings(patch)
    // If screen-sampler settings changed, refresh the live cache so the
    // next tick reads the new blocklist / pause flag without restarting.
    if (patch.screenSampler !== undefined) {
      await refreshScreenSamplerLiveState()
    }
    return next
  })

  // ----- screen sampler IPC -----
  ipcMain.handle('habit:screen-sampler:state', async () => {
    return getScreenSamplerStatus()
  })

  ipcMain.handle('habit:screen-sampler:toggle-pause', async () => {
    return toggleScreenSamplerPause()
  })

  ipcMain.handle(
    'habit:screen-sampler:set-paused',
    async (_e, { paused }: { paused: boolean }) => {
      return setScreenSamplerPaused(!!paused)
    }
  )

  ipcMain.handle('habit:events:recent', async (_e, { limit = 100 }: { limit?: number } = {}) => {
    return { events: listRecentHabitEvents(limit), total: countHabitEvents() }
  })

  ipcMain.handle('habit:events:clear', async () => {
    const removed = clearAllHabitEvents()
    return { ok: true, removed }
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

  // ----- skills library -----
  ipcMain.handle('habit:skills:list', async () => {
    return listSkills()
  })

  ipcMain.handle('habit:skills:get', async (_e, { id }: { id: number }) => {
    return getSkill(id)
  })

  ipcMain.handle('habit:skills:create', async (_e, input: CreateSkillInput) => {
    const id = createSkill(input)
    return { ok: true as const, id }
  })

  ipcMain.handle(
    'habit:skills:update',
    async (_e, { id, patch }: { id: number; patch: UpdateSkillInput }) => {
      updateSkill(id, patch)
      return { ok: true as const }
    }
  )

  ipcMain.handle('habit:skills:delete', async (_e, { id }: { id: number }) => {
    deleteSkill(id)
    return { ok: true as const }
  })

  ipcMain.handle(
    'habit:skills:touch-last-used',
    async (_e, { id }: { id: number }) => {
      touchSkillLastUsed(id)
      return { ok: true as const }
    }
  )

  /** Convenience: accept a candidate by reading its meta + persisting a skill. */
  ipcMain.handle(
    'habit:candidates:accept-as-skill',
    async (
      _e,
      req: {
        candidateId: number
        name: string
        description?: string | null
        trigger?: string | null
        steps: SkillStep[]
      }
    ) => {
      const id = createSkill({
        name: req.name,
        description: req.description ?? null,
        trigger: req.trigger ?? null,
        steps: req.steps,
        source: 'candidate',
        candidateId: req.candidateId
      })
      updateSkillCandidateStatus(req.candidateId, 'accepted')
      return { ok: true as const, id }
    }
  )
}
