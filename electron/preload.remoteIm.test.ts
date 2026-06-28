import { beforeEach, describe, expect, it, vi } from 'vitest'

const electronMock = vi.hoisted(() => ({
  exposed: {} as Record<string, any>,
  invoke: vi.fn(),
  on: vi.fn(),
  removeListener: vi.fn(),
  send: vi.fn(),
  exposeInMainWorld: vi.fn((name: string, api: unknown) => {
    electronMock.exposed[name] = api
  })
}))

vi.mock('electron', () => ({
  contextBridge: {
    exposeInMainWorld: electronMock.exposeInMainWorld
  },
  ipcRenderer: {
    invoke: electronMock.invoke,
    on: electronMock.on,
    removeListener: electronMock.removeListener,
    send: electronMock.send
  },
  webUtils: {
    getPathForFile: vi.fn()
  },
  IpcRendererEvent: class {}
}))

async function loadApi(): Promise<any> {
  vi.resetModules()
  electronMock.exposed = {}
  await import('./preload.js')
  return electronMock.exposed.api
}

beforeEach(() => {
  vi.clearAllMocks()
})

describe('preload remote IM api', () => {
  it('routes remote IM config and message calls through dedicated IPC channels', async () => {
    const api = await loadApi()
    const config = {
      enabled: true,
      provider: 'tencent-im',
      sdkAppId: 1400000000,
      desktopUserId: 'desktop_bot',
      desktopRole: 'master',
      userSigMode: 'endpoint',
      userSigEndpoint: 'https://example.test/sig',
      userSigSecretKey: '',
      friendUserIds: [],
      masterUserIds: ['phone_admin'],
      slaveUserIds: [],
      allowedUserIds: ['phone_admin'],
      outputFlushIntervalMs: 2000,
      outputMaxChunkChars: 1200
    }

    await api.remoteIm.getConfig('project-1')
    await api.remoteIm.getLoginState()
    await api.remoteIm.getAccountByUserId('desktop_slave')
    await api.remoteIm.setAccount(config)
    await api.remoteIm.setConfig('project-1', config)
    await api.remoteIm.getStatus('project-1')
    await api.remoteIm.listMessages('project-1', 50)
    await api.remoteIm.clearMessages('project-1')
    await api.remoteIm.sendLocalMessage('project-1', 'hello')
    await api.remoteIm.sendPeerMessage('project-1', 'hello peer', 'desktop_slave')
    await api.remoteIm.markOutgoingMessageSent('project-1', 42)
    await api.remoteIm.markOutgoingMessageFailed('project-1', 43, 'send failed')
    await api.remoteIm.writeRuntimeLog({
      projectId: 'project-1',
      event: 'send:start',
      desktopUserId: 'desktop_bot'
    })
    await api.remoteIm.deliverIncomingText({
      projectId: 'project-1',
      fromUserId: 'phone_admin',
      text: 'hello'
    })
    await api.remoteIm.updateSdkStatus({
      projectId: 'project-1',
      state: 'connected',
      detail: null
    })

    expect(electronMock.invoke).toHaveBeenNthCalledWith(1, 'remote-im:get-config', {
      projectId: 'project-1'
    })
    expect(electronMock.invoke).toHaveBeenNthCalledWith(2, 'remote-im:get-login-state')
    expect(electronMock.invoke).toHaveBeenNthCalledWith(3, 'remote-im:get-account-by-user-id', {
      userId: 'desktop_slave'
    })
    expect(electronMock.invoke).toHaveBeenNthCalledWith(4, 'remote-im:set-account', {
      account: config
    })
    expect(api.remoteIm.switchProfile).toBeUndefined()
    expect(electronMock.invoke).toHaveBeenNthCalledWith(5, 'remote-im:set-config', {
      projectId: 'project-1',
      config
    })
    expect(electronMock.invoke).toHaveBeenNthCalledWith(6, 'remote-im:get-status', {
      projectId: 'project-1'
    })
    expect(electronMock.invoke).toHaveBeenNthCalledWith(7, 'remote-im:list-messages', {
      projectId: 'project-1',
      limit: 50
    })
    expect(electronMock.invoke).toHaveBeenNthCalledWith(8, 'remote-im:clear-messages', {
      projectId: 'project-1'
    })
    expect(electronMock.invoke).toHaveBeenNthCalledWith(9, 'remote-im:send-local-message', {
      projectId: 'project-1',
      text: 'hello'
    })
    expect(electronMock.invoke).toHaveBeenNthCalledWith(10, 'remote-im:send-peer-message', {
      projectId: 'project-1',
      text: 'hello peer',
      toUserId: 'desktop_slave'
    })
    expect(electronMock.invoke).toHaveBeenNthCalledWith(11, 'remote-im:mark-outgoing-message-sent', {
      projectId: 'project-1',
      messageId: 42
    })
    expect(electronMock.invoke).toHaveBeenNthCalledWith(12, 'remote-im:mark-outgoing-message-failed', {
      projectId: 'project-1',
      messageId: 43,
      error: 'send failed'
    })
    expect(electronMock.invoke).toHaveBeenNthCalledWith(13, 'remote-im:write-runtime-log', {
      entry: {
        projectId: 'project-1',
        event: 'send:start',
        desktopUserId: 'desktop_bot'
      }
    })
    expect(electronMock.invoke).toHaveBeenNthCalledWith(14, 'remote-im:deliver-incoming-text', {
      projectId: 'project-1',
      fromUserId: 'phone_admin',
      text: 'hello'
    })
    expect(electronMock.invoke).toHaveBeenNthCalledWith(15, 'remote-im:update-sdk-status', {
      projectId: 'project-1',
      state: 'connected',
      detail: null
    })
  })

  it('exposes status and message subscriptions', async () => {
    const api = await loadApi()
    const statusCb = vi.fn()
    const messageCb = vi.fn()

    const offStatus = api.remoteIm.onStatus(statusCb)
    const statusHandler = electronMock.on.mock.calls.find(
      (call) => call[0] === 'remote-im:status'
    )?.[1]
    const status = { projectId: 'project-1', state: 'connected', detail: null, updatedAt: 1 }
    statusHandler({}, status)
    expect(statusCb).toHaveBeenCalledWith(status)
    offStatus()
    expect(electronMock.removeListener).toHaveBeenCalledWith('remote-im:status', statusHandler)

    const offMessage = api.remoteIm.onMessagesChanged(messageCb)
    const messageHandler = electronMock.on.mock.calls.find(
      (call) => call[0] === 'remote-im:messages-changed'
    )?.[1]
    const event = { projectId: 'project-1' }
    messageHandler({}, event)
    expect(messageCb).toHaveBeenCalledWith(event)
    offMessage()
    expect(electronMock.removeListener).toHaveBeenCalledWith(
      'remote-im:messages-changed',
      messageHandler
    )

    const outgoingCb = vi.fn()
    const offOutgoing = api.remoteIm.onOutgoingText(outgoingCb)
    const outgoingHandler = electronMock.on.mock.calls.find(
      (call) => call[0] === 'remote-im:outgoing-text'
    )?.[1]
    const outgoing = { projectId: 'project-1', toUserId: 'phone_admin', text: 'hello', messageId: 42 }
    outgoingHandler({}, outgoing)
    expect(outgoingCb).toHaveBeenCalledWith(outgoing)
    offOutgoing()
    expect(electronMock.removeListener).toHaveBeenCalledWith(
      'remote-im:outgoing-text',
      outgoingHandler
    )
  })
})
