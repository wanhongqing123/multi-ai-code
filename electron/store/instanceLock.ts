import { closeSync, mkdirSync, openSync, readFileSync, rmSync, writeSync } from 'fs'
import { join } from 'path'

/**
 * 每账号单实例锁。
 *
 * 同一账号的数据根（baseDir/accounts/<账号>）任一时刻只允许一个进程持有：DB、
 * imcli 桥接、清理等都以账号根为边界，双开同一账号会数据库打架 / imcli 串扰。
 *
 * 机制：用 `wx`(O_EXCL) 原子创建锁文件（内含持有者 pid）。抢锁时若文件已存在，读出
 * pid 判活——持有者存活 → 该账号已在别处打开；持有者已死（崩溃残留）→ 回收陈旧锁并重
 * 建。正常退出通过 releaseInstanceLock() 删除文件。锁文件就在账号根内，同一目录的不同
 * 路径别名（大小写/尾斜杠/8.3/junction）天然指向同一文件，绕开 Windows 路径别名坑。
 *
 * 已知边界：崩溃后其 pid 若被无关进程复用，判活会误判“存活”→ 该账号短期打不开；此时
 * 提示用户确认无其它窗口后删除锁文件即可。此为不引入原生依赖的可接受折中。
 */

const LOCK_FILE = '.instance.lock'

let heldPath: string | null = null

export interface InstanceLockResult {
  ok: boolean
  /** 已被占用时为 true（该账号在另一个存活进程中打开）。 */
  alreadyLocked?: boolean
  error?: string
}

function isPidAlive(pid: number): boolean {
  if (!Number.isInteger(pid) || pid <= 0) return false
  try {
    // signal 0 只做存在性/权限探测，不真正发信号。
    process.kill(pid, 0)
    return true
  } catch (err) {
    // ESRCH = 进程不存在（已死）；EPERM = 存在但无权限（视为存活）。
    return (err as NodeJS.ErrnoException).code === 'EPERM'
  }
}

function tryCreateLock(lockPath: string): boolean {
  try {
    const fd = openSync(lockPath, 'wx')
    try {
      writeSync(fd, `${process.pid}\n${new Date().toISOString()}\n`)
    } catch {
      /* 写 pid 仅用于诊断/判活，失败不影响持锁 */
    }
    closeSync(fd)
    return true
  } catch (err) {
    if ((err as NodeJS.ErrnoException).code === 'EEXIST') return false
    throw err
  }
}

/**
 * 尝试为账号根获取独占锁。重复对同一根 acquire 幂等成功。
 */
export function acquireInstanceLock(accountRoot: string): InstanceLockResult {
  const lockPath = join(accountRoot, LOCK_FILE)
  if (heldPath === lockPath) return { ok: true }
  if (heldPath) {
    // 一个进程只服务一个账号，不应再抢另一个账号的锁。
    return { ok: false, error: 'this process already holds an instance lock' }
  }
  try {
    mkdirSync(accountRoot, { recursive: true })
  } catch {
    /* 目录已存在；创建失败会在 openSync 处暴露 */
  }
  try {
    if (tryCreateLock(lockPath)) {
      heldPath = lockPath
      return { ok: true }
    }
    // EEXIST：检查持有者是否存活
    let holderPid = 0
    try {
      holderPid = Number.parseInt(readFileSync(lockPath, 'utf8').split(/\r?\n/)[0] ?? '', 10)
    } catch {
      /* 读不到 pid 就当作陈旧锁尝试回收 */
    }
    if (isPidAlive(holderPid)) {
      return { ok: false, alreadyLocked: true }
    }
    // 陈旧锁（持有者已死）：回收后重建
    try {
      rmSync(lockPath, { force: true })
    } catch {
      /* ignore */
    }
    if (tryCreateLock(lockPath)) {
      heldPath = lockPath
      return { ok: true }
    }
    // 回收后仍被抢占：当作已占用
    return { ok: false, alreadyLocked: true }
  } catch (err) {
    return { ok: false, error: err instanceof Error ? err.message : String(err) }
  }
}

/** 释放当前持有的账号锁（切换账号 / 退出时调用）。 */
export function releaseInstanceLock(): void {
  if (!heldPath) return
  try {
    rmSync(heldPath, { force: true })
  } catch {
    /* ignore */
  }
  heldPath = null
}
