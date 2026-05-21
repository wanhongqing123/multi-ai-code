import { EventEmitter } from 'events'
import { afterEach, describe, expect, it, vi } from 'vitest'
import {
  createManagedChromeManager,
  findChromeExecutable
} from './managedChrome.js'

class FakeChild extends EventEmitter {
  pid = 4321
  kill = vi.fn(() => true)
}

afterEach(() => {
  vi.restoreAllMocks()
})

describe('findChromeExecutable', () => {
  it('checks the common Windows Chrome install paths in priority order', async () => {
    const exists = vi.fn(async (target: string) => {
      return target === 'C:\\Users\\demo\\AppData\\Local\\Google\\Chrome\\Application\\chrome.exe'
    })

    const resolved = await findChromeExecutable({
      platform: 'win32',
      env: {
        LOCALAPPDATA: 'C:\\Users\\demo\\AppData\\Local',
        PROGRAMFILES: 'C:\\Program Files',
        'PROGRAMFILES(X86)': 'C:\\Program Files (x86)'
      },
      exists
    })

    expect(resolved).toBe('C:\\Users\\demo\\AppData\\Local\\Google\\Chrome\\Application\\chrome.exe')
    expect(exists.mock.calls.map(([target]) => target)).toEqual([
      'C:\\Users\\demo\\AppData\\Local\\Google\\Chrome\\Application\\chrome.exe'
    ])
  })
})

describe('createManagedChromeManager', () => {
  it('starts Chrome with a managed profile and remote debugging port', async () => {
    const spawn = vi.fn(() => new FakeChild())
    const manager = createManagedChromeManager({
      spawn,
      resolveChromePath: vi
        .fn()
        .mockResolvedValue('C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe'),
      getPort: () => 9222,
      getProfileDir: () => 'E:\\managed\\chrome-profile'
    })

    const state = await manager.start()

    expect(spawn).toHaveBeenCalledWith(
      'C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe',
      expect.arrayContaining([
        '--remote-debugging-port=9222',
        '--user-data-dir=E:\\managed\\chrome-profile',
        '--no-first-run',
        '--no-default-browser-check'
      ]),
      expect.objectContaining({
        detached: false,
        shell: false,
        windowsHide: true
      })
    )
    expect(state).toEqual({
      running: true,
      port: 9222,
      profileDir: 'E:\\managed\\chrome-profile',
      pid: 4321,
      lastActiveUrl: null
    })
  })

  it('stops the running Chrome child and clears the manager state', async () => {
    const child = new FakeChild()
    const manager = createManagedChromeManager({
      spawn: vi.fn(() => child),
      resolveChromePath: vi
        .fn()
        .mockResolvedValue('C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe'),
      getPort: () => 9444,
      getProfileDir: () => 'E:\\managed\\chrome-profile'
    })

    await manager.start()
    await manager.stop()

    expect(child.kill).toHaveBeenCalledWith()
    expect(manager.getState()).toEqual({
      running: false,
      port: null,
      profileDir: null,
      pid: null,
      lastActiveUrl: null
    })
  })
})
