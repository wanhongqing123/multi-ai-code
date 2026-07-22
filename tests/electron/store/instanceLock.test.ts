import { existsSync, mkdtempSync, readFileSync, rmSync, writeFileSync, mkdirSync } from 'fs'
import { join } from 'path'
import { tmpdir } from 'os'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'

// instanceLock 用模块级单例保存当前持有的锁，每个用例用 resetModules 拿到全新状态，
// 从而在单进程内模拟“不同进程”的抢锁场景。
async function freshLock() {
  vi.resetModules()
  return import('../../../electron/store/instanceLock.js')
}

describe('per-account instance lock', () => {
  let dir: string
  const lockPath = () => join(dir, '.instance.lock')

  beforeEach(() => {
    dir = mkdtempSync(join(tmpdir(), 'mai-lock-'))
  })
  afterEach(() => {
    rmSync(dir, { recursive: true, force: true })
  })

  it('acquires a free account root and writes the holder pid', async () => {
    const { acquireInstanceLock } = await freshLock()
    const res = acquireInstanceLock(dir)
    expect(res.ok).toBe(true)
    expect(existsSync(lockPath())).toBe(true)
    expect(readFileSync(lockPath(), 'utf8').split(/\r?\n/)[0]).toBe(String(process.pid))
  })

  it('is idempotent for the same account root within one process', async () => {
    const { acquireInstanceLock } = await freshLock()
    expect(acquireInstanceLock(dir).ok).toBe(true)
    expect(acquireInstanceLock(dir).ok).toBe(true)
  })

  it('refuses when a live process already holds the lock', async () => {
    // 预置一个 pid 存活的锁文件（用当前进程 pid，必然存活），模拟另一个进程持有。
    mkdirSync(dir, { recursive: true })
    writeFileSync(lockPath(), `${process.pid}\n2026-01-01T00:00:00.000Z\n`)
    const { acquireInstanceLock } = await freshLock()
    const res = acquireInstanceLock(dir)
    expect(res.ok).toBe(false)
    expect(res.alreadyLocked).toBe(true)
  })

  it('reclaims a stale lock left by a dead process', async () => {
    // 一个几乎不可能存活的 pid（模拟崩溃残留）。
    mkdirSync(dir, { recursive: true })
    writeFileSync(lockPath(), `2147483646\n2026-01-01T00:00:00.000Z\n`)
    const { acquireInstanceLock } = await freshLock()
    const res = acquireInstanceLock(dir)
    expect(res.ok).toBe(true)
    expect(readFileSync(lockPath(), 'utf8').split(/\r?\n/)[0]).toBe(String(process.pid))
  })

  it('releases the lock and removes the file', async () => {
    const { acquireInstanceLock, releaseInstanceLock } = await freshLock()
    expect(acquireInstanceLock(dir).ok).toBe(true)
    expect(existsSync(lockPath())).toBe(true)
    releaseInstanceLock()
    expect(existsSync(lockPath())).toBe(false)
  })
})
