import { ipcMain } from 'electron'
import {
  addSessionDataListener,
  addSessionExitListener,
  getScheduledTaskSessionForProject,
  sendUserMessageToSession
} from '../cc/ptyManager.js'
import {
  cancelScheduledTaskQueueRun,
  drainScheduledTaskQueue,
  enqueueScheduledTaskNow,
  handleScheduledTaskSessionData,
  handleScheduledTaskSessionExit,
  runScheduledTaskScanOnce,
  setScheduledTaskSendHandler
} from './scheduler.js'
import {
  cancelInterruptedScheduledTaskRuns,
  createScheduledTask,
  deleteScheduledTask,
  getScheduledTask,
  listScheduledTasks,
  setScheduledTaskEnabled,
  updateScheduledTask
} from './taskStore.js'
import type { CreateScheduledTaskInput, UpdateScheduledTaskInput } from './types.js'

let removeSessionDataListener: (() => void) | null = null
let removeSessionExitListener: (() => void) | null = null

export function registerScheduledTaskIpc(): void {
  cancelInterruptedScheduledTaskRuns()

  setScheduledTaskSendHandler({
    resolveSession: getScheduledTaskSessionForProject,
    sendUser: sendUserMessageToSession
  })

  if (!removeSessionDataListener) {
    removeSessionDataListener = addSessionDataListener((evt) => {
      handleScheduledTaskSessionData(evt.sessionId, evt.chunk)
    })
  }
  if (!removeSessionExitListener) {
    removeSessionExitListener = addSessionExitListener((evt) => {
      handleScheduledTaskSessionExit(evt.sessionId)
    })
  }

  ipcMain.handle(
    'scheduled-tasks:list',
    async (_event, { projectId }: { projectId: string }) => {
      return listScheduledTasks(projectId)
    }
  )

  ipcMain.handle(
    'scheduled-tasks:create',
    async (_event, input: CreateScheduledTaskInput) => {
      return { ok: true as const, task: createScheduledTask(input) }
    }
  )

  ipcMain.handle(
    'scheduled-tasks:update',
    async (
      _event,
      { id, patch }: { id: number; patch: UpdateScheduledTaskInput }
    ) => {
      updateScheduledTask(id, patch)
      return { ok: true as const, task: getScheduledTask(id) }
    }
  )

  ipcMain.handle('scheduled-tasks:delete', async (_event, { id }: { id: number }) => {
    deleteScheduledTask(id)
    return { ok: true as const }
  })

  ipcMain.handle(
    'scheduled-tasks:set-enabled',
    async (_event, { id, enabled }: { id: number; enabled: boolean }) => {
      setScheduledTaskEnabled(id, enabled)
      return { ok: true as const, task: getScheduledTask(id) }
    }
  )

  ipcMain.handle(
    'scheduled-tasks:run-now',
    async (
      _event,
      {
        taskId,
        sessionId,
        targetRepo
      }: {
        taskId: number
        sessionId?: string | null
        targetRepo?: string | null
      }
    ) => {
      const task = getScheduledTask(taskId)
      if (!task) return { ok: false as const, error: 'scheduled task not found' }

      const projectSession = getScheduledTaskSessionForProject(task.projectId)
      const preferredSession = sessionId
        ? {
            sessionId,
            targetRepo: targetRepo ?? projectSession?.targetRepo ?? 'current project'
          }
        : undefined
      const session = preferredSession ?? projectSession
      if (!session) {
        return {
          ok: false as const,
          error: '主会话未运行，无法发送定时任务。请先启动 AICLI。',
          state: await drainScheduledTaskQueue()
        }
      }
      const result = await enqueueScheduledTaskNow(
        task,
        session?.targetRepo ?? targetRepo ?? 'current project',
        Date.now(),
        preferredSession
      )
      if (result.delivery === 'failed') {
        return {
          ok: false as const,
          error: result.error ?? '发送定时任务到 AICLI 失败。',
          state: result.state
        }
      }
      return {
        ok: true as const,
        delivery: result.delivery,
        queued: result.delivery === 'queued',
        state: result.state
      }
    }
  )

  ipcMain.handle(
    'scheduled-tasks:scan-now',
    async (_event, { projectId }: { projectId?: string } = {}) => {
      return {
        ok: true as const,
        state: await runScheduledTaskScanOnce({ projectId })
      }
    }
  )

  ipcMain.handle('scheduled-tasks:queue-state', async () => {
    return drainScheduledTaskQueue()
  })

  ipcMain.handle(
    'scheduled-tasks:cancel-queue-run',
    async (_event, { runId }: { runId?: number | null } = {}) => {
      return { ok: true as const, ...(await cancelScheduledTaskQueueRun(runId ?? undefined)) }
    }
  )
}
