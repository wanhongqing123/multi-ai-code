import { beforeEach, describe, expect, it, vi } from 'vitest'

const electronMock = vi.hoisted(() => ({
  exposed: {} as Record<string, any>,
  invoke: vi.fn(),
  on: vi.fn(),
  removeListener: vi.fn(),
  send: vi.fn(),
  exposeInMainWorld: vi.fn((name: string, api: unknown) => {
    electronMock.exposed[name] = api
  }),
}))

vi.mock('electron', () => ({
  contextBridge: {
    exposeInMainWorld: electronMock.exposeInMainWorld,
  },
  ipcRenderer: {
    invoke: electronMock.invoke,
    on: electronMock.on,
    removeListener: electronMock.removeListener,
    send: electronMock.send,
  },
  webUtils: {
    getPathForFile: vi.fn(),
  },
  IpcRendererEvent: class {},
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

describe('preload runtime api', () => {
  it('routes project runtime config calls through dedicated IPC channels', async () => {
    const api = await loadApi()
    const runtimeConfig = {
      enabled: true,
      cwd: '.',
      command: 'npm run dev',
      envType: 'msys',
      visualStudioInstanceId: '',
      outputEncoding: 'auto',
    }

    await api.project.getRuntimeConfig('project-1')
    await api.project.setRuntimeConfig('project-1', runtimeConfig)

    expect(electronMock.invoke).toHaveBeenNthCalledWith(1, 'project:get-runtime-config', {
      id: 'project-1',
    })
    expect(electronMock.invoke).toHaveBeenNthCalledWith(2, 'project:set-runtime-config', {
      id: 'project-1',
      config: runtimeConfig,
    })
  })

  it('exposes runtime lifecycle, prompt, and event subscriptions', async () => {
    const api = await loadApi()
    const dataCb = vi.fn()
    const statusCb = vi.fn()

    await api.runtime.start('project-1')
    await api.runtime.stop()
    await api.runtime.getState()
    await api.runtime.getAnalysisPrompt()

    expect(electronMock.invoke).toHaveBeenNthCalledWith(1, 'runtime:start', {
      id: 'project-1',
    })
    expect(electronMock.invoke).toHaveBeenNthCalledWith(2, 'runtime:stop')
    expect(electronMock.invoke).toHaveBeenNthCalledWith(3, 'runtime:get-state')
    expect(electronMock.invoke).toHaveBeenNthCalledWith(4, 'runtime:get-analysis-prompt')

    const offData = api.runtime.onData(dataCb)
    const dataHandler = electronMock.on.mock.calls.find((call) => call[0] === 'runtime:data')?.[1]
    const dataEvent = { stream: 'stdout', chunk: 'hello' }
    dataHandler({}, dataEvent)
    expect(dataCb).toHaveBeenCalledWith(dataEvent)
    offData()
    expect(electronMock.removeListener).toHaveBeenCalledWith('runtime:data', dataHandler)

    const offStatus = api.runtime.onStatus(statusCb)
    const statusHandler = electronMock.on.mock.calls.find(
      (call) => call[0] === 'runtime:status'
    )?.[1]
    const statusEvent = { status: 'running' }
    statusHandler({}, statusEvent)
    expect(statusCb).toHaveBeenCalledWith(statusEvent)
    offStatus()
    expect(electronMock.removeListener).toHaveBeenCalledWith('runtime:status', statusHandler)
  })

  it('exposes a pasted user-message route for large runtime log prompts', async () => {
    const api = await loadApi()

    await api.cc.sendPastedUser('session-1', 'large\nruntime\nlog')

    expect(electronMock.invoke).toHaveBeenCalledWith('cc:send-pasted-user', {
      sessionId: 'session-1',
      text: 'large\nruntime\nlog',
    })
  })
})
