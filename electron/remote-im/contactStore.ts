import type { RemoteImDatabase } from './messageStore.js'

/**
 * Remote IM 好友名单。
 *
 * 存 DB 而不是 JSON：JSON 要整表读写（加一个人得把整个文件读出来改再写回，中途崩溃
 * 就是半个文件），去重只能靠手工 normalize，而且与同库的 remote_im_messages 无法
 * 直接 join。这里 user_id 是主键，重复由数据库拒绝。
 *
 * 与 messageStore 一样走依赖注入：better-sqlite3 是按 Electron ABI 编译的，
 * vitest 跑在系统 Node 上加载不了，注入后才能用假库测出真逻辑。
 */
export interface RemoteImContactStore {
  /** 加好友。已存在则只清墓碑，不改 created_at。 */
  add(userId: string): void
  /** 删好友：保留行并立墓碑，而不是 DELETE——删行会让 SDK 同步把人加回来。 */
  block(userId: string): void
  /** 当前有效好友（不含已删除的），按加入顺序。 */
  list(): string[]
  /** 已被本地删除的账号，供 SDK 同步时过滤。 */
  listBlocked(): string[]
}

export function createRemoteImContactStore(
  db: RemoteImDatabase,
  now: () => number = Date.now
): RemoteImContactStore {
  function upsert(userId: string, blocked: 0 | 1): void {
    const cleanUserId = userId.trim()
    if (!cleanUserId) return
    db.prepare(
      `INSERT INTO remote_im_contacts (user_id, blocked, created_at)
       VALUES (?, ?, ?)
       ON CONFLICT(user_id) DO UPDATE SET blocked = excluded.blocked`
    ).run(cleanUserId, blocked, now())
  }

  function select(blocked: 0 | 1): string[] {
    const rows = db
      .prepare(
        `SELECT user_id FROM remote_im_contacts WHERE blocked = ? ORDER BY created_at, user_id`
      )
      .all(blocked) as Array<{ user_id: string }>
    return rows.map((row) => row.user_id)
  }

  return {
    add: (userId) => upsert(userId, 0),
    block: (userId) => upsert(userId, 1),
    list: () => select(0),
    listBlocked: () => select(1)
  }
}
