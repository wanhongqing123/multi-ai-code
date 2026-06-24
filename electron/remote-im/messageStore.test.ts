import { describe, expect, it } from 'vitest'
import { createRemoteImMessageStore, type RemoteImDatabase } from './messageStore.js'

interface FakeRow {
  id: number
  project_id: string | null
  session_id: string | null
  provider: 'tencent-im'
  remote_message_id: string | null
  from_user_id: string | null
  to_user_id: string | null
  role: 'remote-user' | 'system' | 'aicli'
  direction: 'incoming' | 'outgoing' | 'internal'
  content: string
  status: 'received' | 'rejected' | 'sent-to-aicli' | 'streaming' | 'sent-to-im' | 'failed'
  error: string | null
  created_at: number
  sent_to_aicli_at: number | null
  sent_to_im_at: number | null
}

function createFakeDatabase(): RemoteImDatabase {
  const rows: FakeRow[] = []
  let nextId = 1
  return {
    prepare(sql: string) {
      if (sql.includes('INSERT INTO remote_im_messages')) {
        return {
          run: (...args: unknown[]) => {
            rows.push({
              id: nextId,
              project_id: args[0] as string | null,
              session_id: args[1] as string | null,
              provider: args[2] as 'tencent-im',
              remote_message_id: args[3] as string | null,
              from_user_id: args[4] as string | null,
              to_user_id: args[5] as string | null,
              role: args[6] as FakeRow['role'],
              direction: args[7] as FakeRow['direction'],
              content: args[8] as string,
              status: args[9] as FakeRow['status'],
              error: args[10] as string | null,
              created_at: args[11] as number,
              sent_to_aicli_at: args[12] as number | null,
              sent_to_im_at: args[13] as number | null
            })
            return { lastInsertRowid: nextId++ }
          },
          get: () => undefined,
          all: () => []
        }
      }
      if (sql.includes('SELECT * FROM remote_im_messages WHERE id = ?')) {
        return {
          run: () => ({}),
          get: (id: unknown) => rows.find((row) => row.id === id),
          all: () => []
        }
      }
      if (sql.includes('UPDATE remote_im_messages')) {
        return {
          run: (...args: unknown[]) => {
            const row = rows.find((item) => item.id === args[5])
            if (row) {
              row.session_id = args[0] as string | null
              row.status = args[1] as FakeRow['status']
              row.error = args[2] as string | null
              row.sent_to_aicli_at = args[3] as number | null
              row.sent_to_im_at = args[4] as number | null
            }
            return {}
          },
          get: () => undefined,
          all: () => []
        }
      }
      if (sql.includes('DELETE FROM remote_im_messages')) {
        return {
          run: (projectId: unknown) => {
            for (let index = rows.length - 1; index >= 0; index -= 1) {
              if (rows[index].project_id === projectId) rows.splice(index, 1)
            }
            return {}
          },
          get: () => undefined,
          all: () => []
        }
      }
      if (sql.includes('WHERE project_id = ?')) {
        return {
          run: () => ({}),
          get: () => undefined,
          all: (projectId: unknown, limit: unknown) =>
            rows
              .filter((row) => row.project_id === projectId)
              .sort((left, right) => left.created_at - right.created_at || left.id - right.id)
              .slice(0, Number(limit))
        }
      }
      throw new Error(`Unexpected SQL: ${sql}`)
    }
  }
}

describe('remote IM message store', () => {
  it('creates and lists messages for one project newest last', () => {
    const store = createRemoteImMessageStore(createFakeDatabase())
    store.create({
      projectId: 'project-2',
      sessionId: 'session-other',
      provider: 'tencent-im',
      remoteMessageId: 'remote-0',
      fromUserId: 'phone_other',
      toUserId: 'desktop_bot',
      role: 'remote-user',
      direction: 'incoming',
      content: 'other project',
      status: 'received',
      createdAt: 100
    })
    const first = store.create({
      projectId: 'project-1',
      sessionId: 'session-main',
      provider: 'tencent-im',
      remoteMessageId: 'remote-1',
      fromUserId: 'phone_admin',
      toUserId: 'desktop_bot',
      role: 'remote-user',
      direction: 'incoming',
      content: 'hello',
      status: 'received',
      createdAt: 200
    })
    const second = store.create({
      projectId: 'project-1',
      sessionId: 'session-main',
      provider: 'tencent-im',
      remoteMessageId: null,
      fromUserId: null,
      toUserId: 'phone_admin',
      role: 'system',
      direction: 'outgoing',
      content: 'sent',
      status: 'sent-to-im',
      createdAt: 300,
      sentToImAt: 320
    })

    expect(first.id).toBeGreaterThan(0)
    expect(store.list('project-1', 20).map((message) => message.id)).toEqual([
      first.id,
      second.id
    ])
  })

  it('updates message status and clears project messages', () => {
    const store = createRemoteImMessageStore(createFakeDatabase())
    const message = store.create({
      projectId: 'project-1',
      sessionId: 'session-main',
      provider: 'tencent-im',
      remoteMessageId: 'remote-1',
      fromUserId: 'phone_admin',
      toUserId: 'desktop_bot',
      role: 'remote-user',
      direction: 'incoming',
      content: 'hello',
      status: 'received',
      createdAt: 200
    })

    store.updateStatus(message.id, {
      status: 'sent-to-aicli',
      sentToAicliAt: 250,
      error: null
    })
    expect(store.list('project-1', 20)[0]).toMatchObject({
      id: message.id,
      status: 'sent-to-aicli',
      sentToAicliAt: 250,
      error: null
    })

    store.clear('project-1')
    expect(store.list('project-1', 20)).toEqual([])
  })
})
