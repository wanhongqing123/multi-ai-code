import type { RemoteImConfig, RemoteImIncomingTextMessage } from '../../electron/preload.js'

export interface TencentImTextMessage {
  remoteMessageId: string | null
  fromUserId: string
  toUserId: string | null
  text: string
  createdAt?: number
}

export function extractUserSig(payload: unknown): string {
  if (payload && typeof payload === 'object') {
    const value = payload as { userSig?: unknown }
    if (typeof value.userSig === 'string' && value.userSig.trim()) {
      return value.userSig.trim()
    }
  }
  throw new Error('UserSig endpoint response must include userSig')
}

function getTextPayload(message: Record<string, unknown>): string | null {
  const payload = message.payload
  if (!payload || typeof payload !== 'object') return null
  const text = (payload as { text?: unknown }).text
  return typeof text === 'string' && text.trim() ? text : null
}

export function extractTencentImTextMessages(event: unknown): TencentImTextMessage[] {
  const data = event && typeof event === 'object' ? (event as { data?: unknown }).data : null
  if (!Array.isArray(data)) return []
  return data.flatMap((item) => {
    if (!item || typeof item !== 'object') return []
    const message = item as Record<string, unknown>
    const text = getTextPayload(message)
    if (!text) return []
    const from = typeof message.from === 'string' ? message.from : ''
    if (!from) return []
    return [
      {
        remoteMessageId: typeof message.ID === 'string' ? message.ID : null,
        fromUserId: from,
        toUserId: typeof message.to === 'string' ? message.to : null,
        text,
        createdAt: typeof message.time === 'number' ? message.time * 1000 : undefined
      }
    ]
  })
}

async function requestUserSig(config: RemoteImConfig): Promise<string> {
  const response = await fetch(config.userSigEndpoint, {
    method: 'POST',
    headers: { 'content-type': 'application/json' },
    body: JSON.stringify({
      sdkAppId: config.sdkAppId,
      userId: config.desktopUserId
    })
  })
  if (!response.ok) {
    throw new Error(`UserSig endpoint returned HTTP ${response.status}`)
  }
  return extractUserSig(await response.json())
}

async function loadTencentImSdk(): Promise<any> {
  const moduleName = '@tencentcloud/lite-chat'
  const mod = await import(/* @vite-ignore */ moduleName)
  return mod.default ?? mod.TencentCloudChat ?? mod
}

export interface TencentImRuntime {
  disconnect(): Promise<void>
  sendText(toUserId: string, text: string): Promise<void>
}

export async function connectTencentImClient(input: {
  projectId: string
  config: RemoteImConfig
  onIncomingText: (message: RemoteImIncomingTextMessage) => void
}): Promise<TencentImRuntime> {
  const userSig = await requestUserSig(input.config)
  const TencentCloudChat = await loadTencentImSdk()
  const chat = TencentCloudChat.create({ SDKAppID: input.config.sdkAppId })
  chat.setLogLevel?.(1)

  const onMessageReceived = (event: unknown): void => {
    for (const message of extractTencentImTextMessages(event)) {
      input.onIncomingText({
        projectId: input.projectId,
        remoteMessageId: message.remoteMessageId,
        fromUserId: message.fromUserId,
        toUserId: message.toUserId,
        text: message.text,
        createdAt: message.createdAt
      })
    }
  }

  const eventName = TencentCloudChat.EVENT?.MESSAGE_RECEIVED ?? 'messageReceived'
  chat.on?.(eventName, onMessageReceived)
  await chat.login({
    userID: input.config.desktopUserId,
    userSig
  })

  return {
    async disconnect() {
      chat.off?.(eventName, onMessageReceived)
      await chat.logout?.()
      await chat.destroy?.()
    },
    async sendText(toUserId: string, text: string) {
      const message = chat.createTextMessage({
        to: toUserId,
        conversationType: TencentCloudChat.TYPES?.CONV_C2C ?? 'C2C',
        payload: { text }
      })
      await chat.sendMessage(message)
    }
  }
}
