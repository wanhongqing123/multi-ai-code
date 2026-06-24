import { BrowserWindow, ipcMain } from 'electron'
import { promises as fs } from 'fs'
import { join } from 'path'
import {
  addSessionDataListener,
  addSessionExitListener,
  getActiveSessionForProject,
  sendUserMessageToSession
} from '../cc/ptyManager.js'
import { projectDir } from '../store/paths.js'
import { readProjectMetaFile, writeProjectMetaFile, type ProjectMeta } from '../store/projectMeta.js'
import { DEFAULT_REMOTE_IM_CONFIG, normalizeRemoteImConfig, validateRemoteImConfig } from './config.js'
import {
  clearRemoteImMessages,
  createRemoteImMessage,
  listRemoteImMessages,
  updateRemoteImMessageStatus
} from './messageStore.js'
import { createOutputChunks } from './outputBuffer.js'
import { createRemoteImRouter } from './router.js'
import type { RemoteImConfig, RemoteImIncomingTextMessage, RemoteImStatus } from './types.js'

const REMOTE_IM_META_KEY = 'remote_im_config'

const statuses = new Map<string, RemoteImStatus>()
const outputSessions = new Map<
  string,
  {
    projectId: string
    toUserId: string
    config: RemoteImConfig
    buffer: string
    timer: NodeJS.Timeout | null
  }
>()

function broadcast(channel: string, payload: unknown): void {
  for (const win of BrowserWindow.getAllWindows()) {
    if (!win.isDestroyed()) win.webContents.send(channel, payload)
  }
}

function broadcastStatus(status: RemoteImStatus): void {
  statuses.set(status.projectId ?? '', status)
  broadcast('remote-im:status', status)
}

function broadcastMessagesChanged(projectId: string | null): void {
  broadcast('remote-im:messages-changed', { projectId })
}

function broadcastOutgoingText(projectId: string, toUserId: string, text: string): void {
  broadcast('remote-im:outgoing-text', { projectId, toUserId, text })
}

async function readProjectMeta(projectId: string): Promise<{ meta: ProjectMeta; repaired: boolean }> {
  const metaPath = join(projectDir(projectId), 'project.json')
  try {
    const result = await readProjectMetaFile(metaPath)
    if (!result.ok) return { meta: {}, repaired: false }
    return { meta: result.meta, repaired: result.repaired }
  } catch {
    return { meta: {}, repaired: false }
  }
}

async function writeProjectMeta(projectId: string, meta: ProjectMeta): Promise<void> {
  const dir = projectDir(projectId)
  await fs.mkdir(dir, { recursive: true })
  await writeProjectMetaFile(join(dir, 'project.json'), meta)
}

async function getRemoteImConfig(projectId: string): Promise<RemoteImConfig> {
  const { meta } = await readProjectMeta(projectId)
  return normalizeRemoteImConfig(meta[REMOTE_IM_META_KEY])
}

async function setRemoteImConfig(
  projectId: string,
  rawConfig: unknown
): Promise<
  | { ok: true; value: RemoteImConfig; repaired?: true }
  | { ok: false; error: string; details?: Array<{ path: string; message: string }> }
> {
  const config = normalizeRemoteImConfig(rawConfig)
  const validation = validateRemoteImConfig(config)
  if (!validation.ok) {
    return {
      ok: false,
      error: 'remote IM config is invalid',
      details: validation.issues.map((issue) => ({
        path: issue.path,
        message: issue.message
      }))
    }
  }

  const { meta, repaired } = await readProjectMeta(projectId)
  meta[REMOTE_IM_META_KEY] = config
  await writeProjectMeta(projectId, meta)
  const state = config.enabled ? 'disconnected' : 'disabled'
  broadcastStatus({
    projectId,
    state,
    detail: null,
    updatedAt: Date.now()
  })
  return { ok: true, value: config, ...(repaired ? { repaired: true as const } : {}) }
}

async function getRemoteImStatus(projectId: string): Promise<RemoteImStatus> {
  const existing = statuses.get(projectId)
  if (existing) return existing
  const config = await getRemoteImConfig(projectId)
  return {
    projectId,
    state: config.enabled ? 'disconnected' : 'disabled',
    detail: null,
    updatedAt: Date.now()
  }
}

function sendImText(projectId: string, toUserId: string, text: string): Promise<{ ok: boolean }> {
  broadcastOutgoingText(projectId, toUserId, text)
  return Promise.resolve({ ok: true })
}

function startOutputForwarding(sessionId: string, projectId: string, toUserId: string, config: RemoteImConfig): void {
  const current = outputSessions.get(sessionId)
  if (current?.timer) clearTimeout(current.timer)
  outputSessions.set(sessionId, {
    projectId,
    toUserId,
    config,
    buffer: current?.buffer ?? '',
    timer: null
  })
}

function flushOutputSession(sessionId: string): void {
  const state = outputSessions.get(sessionId)
  if (!state || !state.buffer.trim()) return
  const buffer = state.buffer
  state.buffer = ''
  if (state.timer) {
    clearTimeout(state.timer)
    state.timer = null
  }
  const chunks = createOutputChunks(buffer, {
    maxChunkChars: state.config.outputMaxChunkChars
  })
  for (const chunk of chunks) {
    createRemoteImMessage({
      projectId: state.projectId,
      sessionId,
      provider: 'tencent-im',
      remoteMessageId: null,
      fromUserId: null,
      toUserId: state.toUserId,
      role: 'aicli',
      direction: 'outgoing',
      content: chunk,
      status: 'sent-to-im',
      createdAt: Date.now(),
      sentToImAt: Date.now()
    })
    broadcastOutgoingText(state.projectId, state.toUserId, chunk)
  }
  if (chunks.length > 0) broadcastMessagesChanged(state.projectId)
}

function scheduleOutputFlush(sessionId: string): void {
  const state = outputSessions.get(sessionId)
  if (!state || state.timer) return
  state.timer = setTimeout(() => flushOutputSession(sessionId), state.config.outputFlushIntervalMs)
}

let sessionListenersRegistered = false

function ensureSessionListeners(): void {
  if (sessionListenersRegistered) return
  sessionListenersRegistered = true
  addSessionDataListener(({ sessionId, chunk }) => {
    const state = outputSessions.get(sessionId)
    if (!state) return
    state.buffer += chunk
    scheduleOutputFlush(sessionId)
  })
  addSessionExitListener(({ sessionId }) => {
    flushOutputSession(sessionId)
    outputSessions.delete(sessionId)
  })
}

export function registerRemoteImIpc(): void {
  ensureSessionListeners()

  ipcMain.handle('remote-im:get-config', async (_event, { projectId }: { projectId: string }) => {
    try {
      return { ok: true as const, value: await getRemoteImConfig(projectId) }
    } catch (err) {
      return { ok: false as const, error: err instanceof Error ? err.message : String(err) }
    }
  })

  ipcMain.handle(
    'remote-im:set-config',
    async (_event, { projectId, config }: { projectId: string; config: unknown }) => {
      try {
        return await setRemoteImConfig(projectId, config)
      } catch (err) {
        return { ok: false as const, error: err instanceof Error ? err.message : String(err) }
      }
    }
  )

  ipcMain.handle('remote-im:get-status', async (_event, { projectId }: { projectId: string }) => {
    return await getRemoteImStatus(projectId)
  })

  ipcMain.handle(
    'remote-im:list-messages',
    (_event, { projectId, limit }: { projectId: string; limit?: number }) =>
      listRemoteImMessages(projectId, limit ?? 100)
  )

  ipcMain.handle('remote-im:clear-messages', (_event, { projectId }: { projectId: string }) => {
    clearRemoteImMessages(projectId)
    broadcastMessagesChanged(projectId)
    return { ok: true as const }
  })

  ipcMain.handle(
    'remote-im:update-sdk-status',
    (_event, status: Pick<RemoteImStatus, 'projectId' | 'state' | 'detail'>) => {
      broadcastStatus({
        projectId: status.projectId,
        state: status.state,
        detail: status.detail,
        updatedAt: Date.now()
      })
      return { ok: true as const }
    }
  )

  ipcMain.handle(
    'remote-im:deliver-incoming-text',
    async (_event, message: RemoteImIncomingTextMessage) => {
      const config = await getRemoteImConfig(message.projectId)
      const session = getActiveSessionForProject(message.projectId)
      const router = createRemoteImRouter({
        getConfig: () => config,
        resolveSession: () => session,
        sendUser: sendUserMessageToSession,
        sendImText,
        store: {
          create: (input) => createRemoteImMessage(input),
          updateStatus: (id, patch) =>
            updateRemoteImMessageStatus(id, {
              status: patch.status ?? 'received',
              sessionId: patch.sessionId,
              error: patch.error,
              sentToAicliAt: patch.sentToAicliAt,
              sentToImAt: patch.sentToImAt
            })
        }
      })
      const result = await router.handleIncomingText(message)
      if (result.ok && session) {
        startOutputForwarding(session.sessionId, message.projectId, message.fromUserId, config)
      }
      broadcastMessagesChanged(message.projectId)
      return result
    }
  )

  ipcMain.handle(
    'remote-im:send-local-message',
    async (_event, { projectId, text }: { projectId: string; text: string }) => {
      const session = getActiveSessionForProject(projectId)
      if (!session) return { ok: false as const, error: 'No running AICLI session' }
      const result = await sendUserMessageToSession(session.sessionId, text)
      if (result.ok) {
        createRemoteImMessage({
          projectId,
          sessionId: session.sessionId,
          provider: 'tencent-im',
          role: 'system',
          direction: 'internal',
          content: text,
          status: 'sent-to-aicli',
          createdAt: Date.now(),
          sentToAicliAt: Date.now()
        })
        broadcastMessagesChanged(projectId)
      }
      return result
    }
  )
}
