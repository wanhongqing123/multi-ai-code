import { describe, expect, it } from 'vitest'
import { createRemoteImMessageStore, type RemoteImDatabase } from '../../../electron/remote-im/messageStore.js'

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
  kind?: 'text' | 'image' | 'file'
  attachment_json?: string | null
  status: 'received' | 'rejected' | 'sent-to-aicli' | 'streaming' | 'sent-to-im' | 'failed'
  error: string | null
  created_at: number
  sent_to_aicli_at: number | null
  sent_to_im_at: number | null
}

function createFakeDatabase(seedRows: FakeRow[] = []): RemoteImDatabase {
  const rows: FakeRow[] = [...seedRows]
  let nextId = 1
  return {
    prepare(sql: string) {
      if (sql.includes('INSERT INTO remote_im_messages')) {
        return {
          run: (...args: unknown[]) => {
            const hasAttachmentColumns = args.length >= 16
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
              kind: hasAttachmentColumns ? (args[9] as FakeRow['kind']) : 'text',
              attachment_json: hasAttachmentColumns ? (args[10] as string | null) : null,
              status: (hasAttachmentColumns ? args[11] : args[9]) as FakeRow['status'],
              error: (hasAttachmentColumns ? args[12] : args[10]) as string | null,
              created_at: (hasAttachmentColumns ? args[13] : args[11]) as number,
              sent_to_aicli_at: (hasAttachmentColumns ? args[14] : args[12]) as number | null,
              sent_to_im_at: (hasAttachmentColumns ? args[15] : args[13]) as number | null
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
      if (sql.includes('(from_user_id = ? OR to_user_id = ?)') && sql.includes('created_at < ?')) {
        return {
          run: () => ({}),
          get: () => undefined,
          all: (...args: unknown[]) => {
            const projectId = args[0] as string
            const peer = args[1] as string
            const beforeCreatedAt = args[3] as number
            const beforeId = args[5] as number
            const limit = args[6] as number
            return rows
              .filter(
                (row) =>
                  row.project_id === projectId &&
                  (row.from_user_id === peer || row.to_user_id === peer) &&
                  (row.created_at < beforeCreatedAt ||
                    (row.created_at === beforeCreatedAt && row.id < beforeId))
              )
              .sort((a, b) => b.created_at - a.created_at || b.id - a.id)
              .slice(0, limit)
          }
        }
      }
      if (sql.includes('UPDATE remote_im_messages')) {
        return {
          run: (...args: unknown[]) => {
            const row = rows.find((item) => item.id === args[6])
            if (row) {
              row.session_id = args[0] as string | null
              row.status = args[1] as FakeRow['status']
              row.error = args[2] as string | null
              row.sent_to_aicli_at = args[3] as number | null
              row.sent_to_im_at = args[4] as number | null
              row.remote_message_id = args[5] as string | null
            }
            return {}
          },
          get: () => undefined,
          all: () => []
        }
      }
      if (sql.includes('DELETE FROM remote_im_messages') && sql.includes('from_user_id')) {
        return {
          run: (projectId: unknown, fromUserId: unknown, toUserId: unknown) => {
            for (let index = rows.length - 1; index >= 0; index -= 1) {
              if (
                rows[index].project_id === projectId &&
                (rows[index].from_user_id === fromUserId || rows[index].to_user_id === toUserId)
              ) {
                rows.splice(index, 1)
              }
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
      if (sql.includes('WHERE provider = ?') && sql.includes('remote_message_id = ?')) {
        return {
          run: () => ({}),
          get: (provider: unknown, remoteMessageId: unknown) =>
            rows.find(
              (row) => row.provider === provider && row.remote_message_id === remoteMessageId
            ),
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
              .sort((left, right) => right.created_at - left.created_at || right.id - left.id)
              .slice(0, Number(limit))
              .sort((left, right) => left.created_at - right.created_at || left.id - right.id)
        }
      }
      throw new Error(`Unexpected SQL: ${sql}`)
    }
  }
}

describe('remote IM message store', () => {
  it('maps legacy text messages to text kind without attachment', () => {
    const store = createRemoteImMessageStore(createFakeDatabase())
    const message = store.create({
      projectId: 'project-1',
      provider: 'tencent-im',
      role: 'remote-user',
      direction: 'incoming',
      content: 'hello',
      status: 'received',
      createdAt: 100
    })

    expect(message).toMatchObject({
      content: 'hello',
      kind: 'text',
      attachment: null
    })
  })

  it('dedupes incoming messages by (provider, remoteMessageId) but not null-id messages', () => {
    const store = createRemoteImMessageStore(createFakeDatabase())
    const base = {
      projectId: 'project-1',
      provider: 'tencent-im' as const,
      role: 'remote-user' as const,
      direction: 'incoming' as const,
      status: 'received' as const
    }
    const first = store.create({ ...base, remoteMessageId: 'rm-1', content: '你好', createdAt: 100 })
    // 同一 remoteMessageId 再次入库（模拟 SDK 漫游重放）→ 返回已存在行，不新增
    const dup = store.create({ ...base, remoteMessageId: 'rm-1', content: '你好', createdAt: 200 })
    expect(dup.id).toBe(first.id)
    expect(store.list('project-1')).toHaveLength(1)

    // remoteMessageId 为空（出站/系统消息）不去重，允许多条
    store.create({ ...base, direction: 'outgoing', content: 'a', createdAt: 300 })
    store.create({ ...base, direction: 'outgoing', content: 'b', createdAt: 400 })
    expect(store.list('project-1')).toHaveLength(3)
  })

  it('persists image message attachments', () => {
    const store = createRemoteImMessageStore(createFakeDatabase())
    const attachment = {
      type: 'image',
      localPath: '/tmp/remote-im/image-1.png',
      remoteUrl: 'https://example.com/image-1.png',
      thumbnailUrl: 'https://example.com/thumb-1.png',
      width: 320,
      height: 240,
      sizeBytes: 2048,
      fileName: 'image-1.png',
      mimeType: 'image/png',
      sdkImageId: 'image-sdk-id'
    }
    const message = store.create({
      projectId: 'project-1',
      provider: 'tencent-im',
      role: 'remote-user',
      direction: 'incoming',
      content: '[图片消息] image-1.png',
      status: 'received',
      createdAt: 100,
      kind: 'image',
      attachment
    } as any)

    expect(store.listById(message.id)).toMatchObject({
      kind: 'image',
      attachment
    })
  })

  it('persists markdown/html file message attachments', () => {
    const store = createRemoteImMessageStore(createFakeDatabase())
    const attachment = {
      type: 'file',
      localPath: '/tmp/remote-im/report.md',
      remoteUrl: 'https://example.com/report.md',
      sizeBytes: 2048,
      fileName: 'report.md',
      mimeType: 'text/markdown',
      sdkFileId: 'file-sdk-id'
    }
    const message = store.create({
      projectId: 'project-1',
      provider: 'tencent-im',
      role: 'remote-user',
      direction: 'incoming',
      content: '[文件消息] report.md',
      status: 'received',
      createdAt: 100,
      kind: 'file',
      attachment
    } as any)

    expect(store.listById(message.id)).toMatchObject({
      kind: 'file',
      attachment
    })
  })

  it('ignores malformed attachment json while preserving image kind', () => {
    const store = createRemoteImMessageStore(
      createFakeDatabase([
        {
          id: 1,
          project_id: 'project-1',
          session_id: null,
          provider: 'tencent-im',
          remote_message_id: null,
          from_user_id: 'phone',
          to_user_id: 'desktop',
          role: 'remote-user',
          direction: 'incoming',
          content: '[图片消息]',
          kind: 'image',
          attachment_json: '{bad-json',
          status: 'received',
          error: null,
          created_at: 100,
          sent_to_aicli_at: null,
          sent_to_im_at: null
        }
      ])
    )

    expect(store.listById(1)).toMatchObject({
      kind: 'image',
      attachment: null
    })
  })

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

  it('lists the latest messages when the project has more rows than the limit', () => {
    const store = createRemoteImMessageStore(createFakeDatabase())
    const first = store.create({
      projectId: 'project-1',
      provider: 'tencent-im',
      role: 'remote-user',
      direction: 'incoming',
      content: 'oldest',
      status: 'received',
      createdAt: 100
    })
    const second = store.create({
      projectId: 'project-1',
      provider: 'tencent-im',
      role: 'remote-user',
      direction: 'incoming',
      content: 'middle',
      status: 'received',
      createdAt: 200
    })
    const third = store.create({
      projectId: 'project-1',
      provider: 'tencent-im',
      role: 'remote-user',
      direction: 'incoming',
      content: 'newest',
      status: 'received',
      createdAt: 300
    })

    expect(first.id).toBeGreaterThan(0)
    expect(store.list('project-1', 2).map((message) => message.id)).toEqual([
      second.id,
      third.id
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

  it('clears only the selected peer conversation history in one project', () => {
    const store = createRemoteImMessageStore(createFakeDatabase())
    const deletedIncoming = store.create({
      projectId: 'project-1',
      provider: 'tencent-im',
      fromUserId: 'friend-a',
      toUserId: 'desktop-bot',
      role: 'remote-user',
      direction: 'incoming',
      content: 'delete incoming',
      status: 'received',
      createdAt: 100
    })
    const deletedOutgoing = store.create({
      projectId: 'project-1',
      provider: 'tencent-im',
      fromUserId: 'desktop-bot',
      toUserId: 'friend-a',
      role: 'remote-user',
      direction: 'outgoing',
      content: 'delete outgoing',
      status: 'sent-to-im',
      createdAt: 110
    })
    const kept = store.create({
      projectId: 'project-1',
      provider: 'tencent-im',
      fromUserId: 'friend-b',
      toUserId: 'desktop-bot',
      role: 'remote-user',
      direction: 'incoming',
      content: 'keep',
      status: 'received',
      createdAt: 120
    })
    const keptOtherProject = store.create({
      projectId: 'project-2',
      provider: 'tencent-im',
      fromUserId: 'friend-a',
      toUserId: 'desktop-bot',
      role: 'remote-user',
      direction: 'incoming',
      content: 'other project',
      status: 'received',
      createdAt: 130
    })

    store.clearPeer('project-1', ' friend-a ')

    expect(store.list('project-1', 20).map((message) => message.id)).toEqual([kept.id])
    expect(store.listById(deletedIncoming.id)).toBeNull()
    expect(store.listById(deletedOutgoing.id)).toBeNull()
    expect(store.listById(keptOtherProject.id)).toMatchObject({ id: keptOtherProject.id })
  })

  it('fails an outgoing message only while it is still streaming', () => {
    const store = createRemoteImMessageStore(createFakeDatabase())
    const message = store.create({
      projectId: 'project-1',
      provider: 'tencent-im',
      role: 'remote-user',
      direction: 'outgoing',
      content: 'hello',
      status: 'streaming',
      createdAt: 200
    })

    expect(store.failIfStreaming(message.id, 'not confirmed')).toMatchObject({
      id: message.id,
      status: 'failed',
      error: 'not confirmed'
    })

    store.updateStatus(message.id, {
      status: 'sent-to-im',
      error: null,
      sentToImAt: 300
    })
    expect(store.failIfStreaming(message.id, 'late timeout')).toMatchObject({
      id: message.id,
      status: 'sent-to-im',
      error: null
    })
  })

  it('backfills remoteMessageId on updateStatus and keeps it when omitted', () => {
    const store = createRemoteImMessageStore(createFakeDatabase())
    const message = store.create({
      projectId: 'project-1',
      sessionId: null,
      provider: 'tencent-im',
      remoteMessageId: null,
      fromUserId: 'desktop_bot',
      toUserId: 'phone_admin',
      role: 'remote-user',
      direction: 'outgoing',
      content: 'hello',
      kind: 'text',
      attachment: null,
      status: 'streaming',
      error: null,
      createdAt: 1,
      sentToAicliAt: null,
      sentToImAt: null
    })

    // 发送成功：SDK 确认的消息 id 回填，漫游重投可据此去重。
    const sent = store.updateStatus(message.id, {
      status: 'sent-to-im',
      sentToImAt: 2,
      remoteMessageId: 'tim-msg-1'
    })
    expect(sent).toMatchObject({ status: 'sent-to-im', remoteMessageId: 'tim-msg-1' })

    // 后续不带 remoteMessageId 的状态更新不能抹掉已回填的 id。
    const touched = store.updateStatus(message.id, { status: 'sent-to-im' })
    expect(touched).toMatchObject({ remoteMessageId: 'tim-msg-1' })

    // 回填后，同 id 的漫游消息 create 命中去重，不再重复入库。
    const duplicate = store.create({
      projectId: 'project-1',
      sessionId: null,
      provider: 'tencent-im',
      remoteMessageId: 'tim-msg-1',
      fromUserId: 'desktop_bot',
      toUserId: 'phone_admin',
      role: 'remote-user',
      direction: 'outgoing',
      content: 'hello',
      kind: 'text',
      attachment: null,
      status: 'sent-to-im',
      error: null,
      createdAt: 1,
      sentToAicliAt: null,
      sentToImAt: 2
    })
    expect(duplicate.id).toBe(message.id)
  })

  it('pages backward per peer with a keyset anchor', () => {
    const store = createRemoteImMessageStore(createFakeDatabase())
    for (let index = 1; index <= 5; index += 1) {
      store.create({
        projectId: 'project-1',
        sessionId: null,
        provider: 'tencent-im',
        remoteMessageId: `roam-${index}`,
        fromUserId: 'phone_admin',
        toUserId: 'desktop_bot',
        role: 'remote-user',
        direction: 'incoming',
        content: `msg-${index}`,
        kind: 'text',
        attachment: null,
        status: 'received',
        error: null,
        createdAt: index * 100,
        sentToAicliAt: null,
        sentToImAt: null
      })
    }

    // 锚点 = 第 4 条（createdAt 400, id 4）：严格早于它的是前 3 条，取 2 条为最近两条（升序返回）。
    const page = store.listPeerBefore('project-1', 'phone_admin', 400, 4, 2)
    expect(page.map((message) => message.content)).toEqual(['msg-2', 'msg-3'])

    // 翻到最早之前：没有更多。
    expect(store.listPeerBefore('project-1', 'phone_admin', 100, 1, 2)).toEqual([])
    // 其他会话不可见。
    expect(store.listPeerBefore('project-1', 'someone_else', 400, 4, 2)).toEqual([])
  })
})
