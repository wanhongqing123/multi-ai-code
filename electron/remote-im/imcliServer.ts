import { createServer, type IncomingMessage, type Server, type ServerResponse } from 'node:http'
import { randomBytes } from 'node:crypto'
import { promises as fs } from 'node:fs'
import { join } from 'node:path'
import type { RemoteImConfig, RemoteImMessage, RemoteImStatus } from './types.js'

export const REMOTE_IM_CLI_BRIDGE_FILE = 'imcli-bridge.json'

export interface RemoteImCliSendResult {
  ok: boolean
  error?: string
  toUserId?: string
}

export interface RemoteImCliServerDeps {
  rootDir: string
  getConfig(projectId: string): Promise<RemoteImConfig>
  getStatus(projectId: string): Promise<RemoteImStatus>
  listMessages(projectId: string, limit?: number): RemoteImMessage[]
  sendPeerMessage(projectId: string, text: string, toUserId?: string | null): Promise<RemoteImCliSendResult>
}

export interface RemoteImCliServerHandle {
  url: string
  token: string
  close(): Promise<void>
}

function json(res: ServerResponse, statusCode: number, payload: unknown): void {
  res.writeHead(statusCode, {
    'content-type': 'application/json; charset=utf-8',
    'cache-control': 'no-store'
  })
  res.end(JSON.stringify(payload))
}

function readBody(req: IncomingMessage): Promise<unknown> {
  return new Promise((resolve, reject) => {
    let body = ''
    req.setEncoding('utf8')
    req.on('data', (chunk) => {
      body += chunk
      if (body.length > 1024 * 1024) {
        reject(new Error('request body too large'))
        req.destroy()
      }
    })
    req.on('end', () => {
      if (!body.trim()) {
        resolve({})
        return
      }
      try {
        resolve(JSON.parse(body))
      } catch {
        reject(new Error('invalid JSON body'))
      }
    })
    req.on('error', reject)
  })
}

function getProjectId(url: URL, body?: unknown): string {
  const fromQuery = url.searchParams.get('projectId')?.trim()
  if (fromQuery) return fromQuery
  if (body && typeof body === 'object') {
    const raw = (body as { projectId?: unknown }).projectId
    if (typeof raw === 'string' && raw.trim()) return raw.trim()
  }
  throw new Error('projectId is required')
}

function uniqueUserIds(userIds: Array<string | null | undefined>): string[] {
  return Array.from(new Set(userIds.map((item) => item?.trim()).filter(Boolean) as string[]))
}

function contactsFromConfig(config: RemoteImConfig): Array<{ userId: string }> {
  return uniqueUserIds([
    ...config.friendUserIds,
    ...config.masterUserIds,
    ...config.slaveUserIds,
    ...config.allowedUserIds
  ])
    .filter((userId) => userId !== config.desktopUserId)
    .map((userId) => ({ userId }))
}

function messagePeerUserId(message: RemoteImMessage, localUserId: string): string | null {
  const from = message.fromUserId?.trim()
  const to = message.toUserId?.trim()
  if (from && from !== localUserId) return from
  if (to && to !== localUserId) return to
  return from || to || null
}

function formatMessage(message: RemoteImMessage): Record<string, unknown> {
  return {
    id: message.id,
    role: message.role,
    direction: message.direction,
    fromUserId: message.fromUserId,
    toUserId: message.toUserId,
    status: message.status,
    content: message.content,
    createdAt: message.createdAt
  }
}

async function writeBridgeFile(rootDir: string, url: string, token: string): Promise<void> {
  await fs.mkdir(rootDir, { recursive: true })
  await fs.writeFile(
    join(rootDir, REMOTE_IM_CLI_BRIDGE_FILE),
    JSON.stringify({ url, token, updatedAt: Date.now() }, null, 2),
    'utf8'
  )
}

function closeServer(server: Server): Promise<void> {
  return new Promise((resolve, reject) => {
    server.close((error) => (error ? reject(error) : resolve()))
  })
}

export async function startRemoteImCliServer(
  deps: RemoteImCliServerDeps
): Promise<RemoteImCliServerHandle> {
  const token = randomBytes(24).toString('hex')
  let server: Server

  server = createServer(async (req, res) => {
    try {
      if (req.headers.authorization !== `Bearer ${token}`) {
        json(res, 401, { ok: false, error: 'unauthorized' })
        return
      }

      const url = new URL(req.url ?? '/', 'http://127.0.0.1')
      if (req.method === 'GET' && url.pathname === '/whoami') {
        const projectId = getProjectId(url)
        const [config, status] = await Promise.all([
          deps.getConfig(projectId),
          deps.getStatus(projectId)
        ])
        json(res, 200, {
          ok: true,
          value: {
            projectId,
            userId: config.desktopUserId,
            sdkAppId: config.sdkAppId,
            status: status.state,
            statusDetail: status.detail
          }
        })
        return
      }

      if (req.method === 'GET' && url.pathname === '/contacts') {
        const projectId = getProjectId(url)
        const config = await deps.getConfig(projectId)
        json(res, 200, {
          ok: true,
          value: {
            projectId,
            contacts: contactsFromConfig(config)
          }
        })
        return
      }

      if (req.method === 'GET' && url.pathname === '/history') {
        const projectId = getProjectId(url)
        const peer = url.searchParams.get('peer')?.trim()
        const limit = Math.max(1, Math.min(200, Number(url.searchParams.get('limit')) || 20))
        const config = await deps.getConfig(projectId)
        const messages = deps
          .listMessages(projectId, limit)
          .filter((message) => !peer || messagePeerUserId(message, config.desktopUserId) === peer)
          .map(formatMessage)
        json(res, 200, { ok: true, value: { projectId, messages } })
        return
      }

      if (req.method === 'POST' && url.pathname === '/send') {
        const body = await readBody(req)
        const projectId = getProjectId(url, body)
        const raw = body && typeof body === 'object' ? (body as Record<string, unknown>) : {}
        const toUserId = typeof raw.toUserId === 'string' ? raw.toUserId.trim() : ''
        const text = typeof raw.text === 'string' ? raw.text.trim() : ''
        if (!toUserId) throw new Error('toUserId is required')
        if (!text) throw new Error('text is required')
        const result = await deps.sendPeerMessage(projectId, text, toUserId)
        json(res, result.ok ? 200 : 400, {
          ok: result.ok,
          ...(result.ok
            ? { value: { toUserId: result.toUserId ?? toUserId } }
            : { error: result.error ?? 'failed to send IM message' })
        })
        return
      }

      json(res, 404, { ok: false, error: 'not found' })
    } catch (err) {
      json(res, 400, { ok: false, error: err instanceof Error ? err.message : String(err) })
    }
  })

  await new Promise<void>((resolve, reject) => {
    server.once('error', reject)
    server.listen(0, '127.0.0.1', () => {
      server.off('error', reject)
      resolve()
    })
  })

  const address = server.address()
  if (!address || typeof address === 'string') {
    await closeServer(server)
    throw new Error('Remote IM CLI bridge did not bind to a TCP port')
  }
  const url = `http://127.0.0.1:${address.port}`
  await writeBridgeFile(deps.rootDir, url, token)

  return {
    url,
    token,
    close: () => closeServer(server)
  }
}
