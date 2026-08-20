import { describe, expect, it } from 'vitest'
import { createRemoteImContactStore } from '../../../electron/remote-im/contactStore.js'
import type { RemoteImDatabase } from '../../../electron/remote-im/messageStore.js'

interface Row {
  user_id: string
  blocked: number
  created_at: number
}

/**
 * 假库：只实现这张表用到的两条 SQL。真 better-sqlite3 是按 Electron ABI 编译的，
 * vitest 跑在系统 Node 上加载不了（taskStore.test.ts 正因如此长期挂在 Windows 上）。
 * 这里连主键冲突的语义一起模拟，才能测出「重复添加不产生重复行」。
 */
function createFakeDatabase(): RemoteImDatabase & { rows: Row[] } {
  const rows: Row[] = []
  return {
    rows,
    prepare(sql: string) {
      if (sql.includes('INSERT INTO remote_im_contacts')) {
        return {
          run: (...args: unknown[]) => {
            const [userId, blocked, createdAt] = args as [string, number, number]
            const existing = rows.find((row) => row.user_id === userId)
            if (existing) {
              existing.blocked = blocked // ON CONFLICT DO UPDATE：不改 created_at
              return {}
            }
            rows.push({ user_id: userId, blocked, created_at: createdAt })
            return {}
          },
          get: () => undefined,
          all: () => []
        }
      }
      if (sql.includes('SELECT user_id FROM remote_im_contacts')) {
        return {
          run: () => ({}),
          get: () => undefined,
          all: (...args: unknown[]) => {
            const [blocked] = args as [number]
            return rows
              .filter((row) => row.blocked === blocked)
              .sort((a, b) => a.created_at - b.created_at || a.user_id.localeCompare(b.user_id))
              .map((row) => ({ user_id: row.user_id }))
          }
        }
      }
      throw new Error(`unexpected SQL: ${sql}`)
    }
  }
}

describe('remote IM contact store', () => {
  it('adds contacts and lists them in insertion order', () => {
    let clock = 100
    const store = createRemoteImContactStore(createFakeDatabase(), () => clock++)
    store.add('phone-user')
    store.add('mac-agent')

    expect(store.list()).toEqual(['phone-user', 'mac-agent'])
  })

  it('lets the primary key reject duplicates instead of hand-rolled dedup', () => {
    const db = createFakeDatabase()
    const store = createRemoteImContactStore(db)
    store.add('phone-user')
    store.add('phone-user')
    store.add('  phone-user  ')

    expect(store.list()).toEqual(['phone-user'])
    expect(db.rows).toHaveLength(1)
  })

  it('ignores blank ids so an empty contact never reaches the table', () => {
    const db = createFakeDatabase()
    const store = createRemoteImContactStore(db)
    store.add('   ')
    store.add('')
    store.block('  ')

    expect(db.rows).toEqual([])
  })

  it('keeps a tombstone on delete so SDK sync cannot resurrect the contact', () => {
    const db = createFakeDatabase()
    const store = createRemoteImContactStore(db)
    store.add('phone-user')
    store.block('phone-user')

    // 行必须保留：直接 DELETE 的话 SDK 下次同步会把这个人重新加回来。
    expect(store.list()).toEqual([])
    expect(store.listBlocked()).toEqual(['phone-user'])
    expect(db.rows).toHaveLength(1)
  })

  it('clears the tombstone when the contact is added back', () => {
    const store = createRemoteImContactStore(createFakeDatabase())
    store.add('phone-user')
    store.block('phone-user')
    store.add('phone-user')

    expect(store.list()).toEqual(['phone-user'])
    expect(store.listBlocked()).toEqual([])
  })

  it('can block an account that was never added, so a stranger stays filtered', () => {
    const store = createRemoteImContactStore(createFakeDatabase())
    store.block('never-added')

    expect(store.list()).toEqual([])
    expect(store.listBlocked()).toEqual(['never-added'])
  })
})
