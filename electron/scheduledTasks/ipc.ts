import { ipcMain } from 'electron'
import {
  addSessionDataListener,
  getSessionForProject,
  sendUserMessageToSession
} from '../cc/ptyManager.js'
import {
  drainScheduledTaskQueue,
  enqueueScheduledTaskNow,
  handleScheduledTaskSessionData,
  runScheduledTaskScanOnce,
  setScheduledTaskSendHandler
} from './scheduler.js'
import {
  createScheduledTask,
  deleteScheduledTask,
  getScheduledTask,
  listScheduledTasks,
  setScheduledTaskEnabled,
  updateScheduledTask
} from './taskStore.js'
import type { CreateScheduledTaskInput, UpdateScheduledTaskInput } from './types.js'

let removeSessionDataListener: (() => void) | null = null

export function registerScheduledTaskIpc(): void {
  setScheduledTaskSendHandler({
    resolveSession: getSessionForProject,
    sendUser: sendUserMessageToSession
  })

  if (!removeSessionDataListener) {
    removeSessionDataListener = addSessionDataListener((evt) => {
      handleScheduledTaskSessionData(evt.sessionId, evt.chunk)
    })
  }

  ipcMain.handle('scheduled-tasks:list', async (_event, { projectId }: { projectId: string }) => {
    return listScheduledTasks(projectId)
  })

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

      const projectSession = getSessionForProject(task.projectId)
      const preferredSession = sessionId
        ? {
            sessionId,
            targetRepo: targetRepo ?? projectSession?.targetRepo ?? 'current project'
          }
        : undefined
      const session = preferredSession ?? projectSession
      const state = await enqueueScheduledTaskNow(
        task,
        session?.targetRepo ?? targetRepo ?? 'current project',
        Date.now(),
        preferredSession
      )
      return {
        ok: true as const,
        queued: !session,
        state
      }
    }
  )

  ipcMain.handle(
    'scheduled-tasks:scan-now',
    async (_event, { projectId }: { projectId?: string } = {}) => {
      return { ok: true as const, state: await runScheduledTaskScanOnce({ projectId }) }
    }
  )

  ipcMain.handle('scheduled-tasks:queue-state', async () => {
    return drainScheduledTaskQueue()
  })
}
