import { ipcMain } from 'electron'
import { getSessionForProject, sendUserMessageToSession } from '../cc/ptyManager.js'
import {
  enqueueScheduledTaskNow,
  getScheduledTaskQueueState,
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

export function registerScheduledTaskIpc(): void {
  setScheduledTaskSendHandler({
    resolveSession: getSessionForProject,
    sendUser: sendUserMessageToSession
  })

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
        targetRepo
      }: {
        taskId: number
        sessionId?: string | null
        targetRepo?: string | null
      }
    ) => {
      const task = getScheduledTask(taskId)
      if (!task) return { ok: false as const, error: '未找到定时任务' }
      const session = getSessionForProject(task.projectId)
      const state = await enqueueScheduledTaskNow(
        task,
        session?.targetRepo ?? targetRepo ?? '当前项目'
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
    return getScheduledTaskQueueState()
  })
}
