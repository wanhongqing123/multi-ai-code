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
      userSigEndpoint: 'https://example.test/sig',
      allowedUserIds: ['phone_admin'],
      outputFlushIntervalMs: 2000,
      outputMaxChunkChars: 1200
    }

    await api.remoteIm.getConfig('project-1')
    await api.remoteIm.setConfig('project-1', config)
    await api.remoteIm.getStatus('project-1')
    await api.remoteIm.listMessages('project-1', 50)
    await api.remoteIm.clearMessages('project-1')
    await api.remoteIm.sendLocalMessage('project-1', 'hello')
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
    expect(electronMock.invoke).toHaveBeenNthCalledWith(2, 'remote-im:set-config', {
      projectId: 'project-1',
      config
    })
    expect(electronMock.invoke).toHaveBeenNthCalledWith(3, 'remote-im:get-status', {
      projectId: 'project-1'
    })
    expect(electronMock.invoke).toHaveBeenNthCalledWith(4, 'remote-im:list-messages', {
      projectId: 'project-1',
      limit: 50
    })
    expect(electronMock.invoke).toHaveBeenNthCalledWith(5, 'remote-im:clear-messages', {
      projectId: 'project-1'
    })
    expect(electronMock.invoke).toHaveBeenNthCalledWith(6, 'remote-im:send-local-message', {
      projectId: 'project-1',
      text: 'hello'
    })
    expect(electronMock.invoke).toHaveBeenNthCalledWith(7, 'remote-im:deliver-incoming-text', {
      projectId: 'project-1',
      fromUserId: 'phone_admin',
      text: 'hello'
    })
    expect(electronMock.invoke).toHaveBeenNthCalledWith(8, 'remote-im:update-sdk-status', {
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
    const outgoing = { projectId: 'project-1', toUserId: 'phone_admin', text: 'hello' }
    outgoingHandler({}, outgoing)
    expect(outgoingCb).toHaveBeenCalledWith(outgoing)
    offOutgoing()
    expect(electronMock.removeListener).toHaveBeenCalledWith(
      'remote-im:outgoing-text',
      outgoingHandler
    )
  })
})
