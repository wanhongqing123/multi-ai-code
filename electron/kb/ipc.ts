import { ipcMain } from 'electron'
import {
  clearKbForRepo,
  deleteKbEntry,
  getKbEntry,
  getKbMeta,
  listKbEntries,
  listKbTopics,
  searchKb,
  touchKbAccess,
  updateKbEntry,
  type KbTier
} from './db.js'
import {
  kbStats,
  projectSchedulerSignals,
  runKbSummaryOnce
} from './runner.js'
import { describeSkipReason, decideKbRun } from './scheduler.js'
import { runCompaction } from './compactor.js'
import { enqueueCliJob } from '../util/cliQueue.js'
import {
  CLI_TIMEOUT_MS,
  runCliGeneration
} from '../habit/generator.js'
import { buildMergePrompt, parseMergeResponse } from './compactor.js'
import type { MergeFn } from './compactor.js'

interface RawAiSettings {
  ai_cli?: 'claude' | 'codex'
  command?: string
  args?: string[]
  env?: Record<string, string>
}

let resolveAiSettingsForRepo: (repoPath: string) => RawAiSettings | null = () =>
  null

/** Lets main.ts inject the AI settings resolver (knows about projects table). */
export function configureKbSettingsResolver(
  fn: (repoPath: string) => RawAiSettings | null
): void {
  resolveAiSettingsForRepo = fn
}

export function registerKbIpc(): void {
  ipcMain.handle('kb:list', async (_e, { repoPath, tier, limit }: {
    repoPath: string
    tier?: KbTier
    limit?: number
  }) => {
    return listKbEntries(repoPath, { tier, limit })
  })

  ipcMain.handle('kb:topics', async (_e, { repoPath }: { repoPath: string }) => {
    return listKbTopics(repoPath)
  })

  ipcMain.handle(
    'kb:search',
    async (
      _e,
      { repoPath, query, limit }: { repoPath: string; query: string; limit?: number }
    ) => {
      const results = searchKb(repoPath, query, limit ?? 20)
      // Touch access count for retrieved entries so importance scoring sees usage.
      for (const r of results) touchKbAccess(repoPath, r.entry.id)
      return results
    }
  )

  ipcMain.handle('kb:get', async (_e, { repoPath, id }: { repoPath: string; id: number }) => {
    return getKbEntry(repoPath, id)
  })

  ipcMain.handle('kb:meta', async (_e, { repoPath }: { repoPath: string }) => {
    return getKbMeta(repoPath)
  })

  ipcMain.handle('kb:stats', async (_e, { repoPath }: { repoPath: string }) => {
    return kbStats(repoPath)
  })

  ipcMain.handle(
    'kb:update',
    async (
      _e,
      req: {
        repoPath: string
        id: number
        topic?: string
        summary?: string
        importance?: number
        tier?: KbTier
      }
    ) => {
      updateKbEntry(req.repoPath, req.id, {
        topic: req.topic,
        summary: req.summary,
        importance: req.importance,
        tier: req.tier
      })
      return { ok: true as const }
    }
  )

  ipcMain.handle(
    'kb:pin',
    async (_e, { repoPath, id }: { repoPath: string; id: number }) => {
      updateKbEntry(repoPath, id, { tier: 'pinned' })
      return { ok: true as const }
    }
  )

  ipcMain.handle(
    'kb:unpin',
    async (_e, { repoPath, id }: { repoPath: string; id: number }) => {
      // Unpinning drops the entry back to warm (its hot slot is gone by now).
      updateKbEntry(repoPath, id, { tier: 'warm' })
      return { ok: true as const }
    }
  )

  ipcMain.handle(
    'kb:delete',
    async (_e, { repoPath, id }: { repoPath: string; id: number }) => {
      deleteKbEntry(repoPath, id)
      return { ok: true as const }
    }
  )

  ipcMain.handle('kb:clear', async (_e, { repoPath }: { repoPath: string }) => {
    const removed = await clearKbForRepo(repoPath)
    return { ok: true as const, removed }
  })

  ipcMain.handle(
    'kb:run-now',
    async (_e, { repoPath }: { repoPath: string }) => {
      try {
        const outcome = await runKbSummaryOnce(repoPath)
        return { ok: true as const, outcome }
      } catch (err) {
        return {
          ok: false as const,
          error: err instanceof Error ? err.message : String(err)
        }
      }
    }
  )

  ipcMain.handle(
    'kb:compact-now',
    async (_e, { repoPath }: { repoPath: string }) => {
      const settings = resolveAiSettingsForRepo(repoPath)
      if (!settings) {
        return { ok: false as const, error: 'no AI settings configured' }
      }
      const merge: MergeFn = async (entries, target) => {
        try {
          const res = await enqueueCliJob('kb-compact', () =>
            runCliGeneration(
              buildMergePrompt(entries, target),
              settings,
              { timeoutMs: CLI_TIMEOUT_MS }
            )
          )
          if (!res.ok || !res.template) return null
          return parseMergeResponse(
            (res.template.body ?? res.template.title ?? '').toString()
          )
        } catch {
          return null
        }
      }
      try {
        const result = await runCompaction(repoPath, merge)
        return { ok: true as const, result }
      } catch (err) {
        return {
          ok: false as const,
          error: err instanceof Error ? err.message : String(err)
        }
      }
    }
  )

  ipcMain.handle(
    'kb:scheduler-status',
    async (_e, { repoPath }: { repoPath: string }) => {
      const signals = projectSchedulerSignals(repoPath)
      const verdict = decideKbRun(signals, Date.now())
      return {
        signals,
        nextActionable: verdict.run ? null : describeSkipReason(verdict.reason),
        willRunReason: verdict.run ? verdict.reason : null
      }
    }
  )
}
