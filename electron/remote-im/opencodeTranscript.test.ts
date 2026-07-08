import { mkdtempSync, writeFileSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { describe, expect, it, vi } from 'vitest'

type MockRow = {
  session_id: string
  time_created: number
  directory: string
  data: string
}

const rowsByDbPath = new Map<string, MockRow[]>()

vi.mock('better-sqlite3', () => {
  class MockDatabase {
    readonly file: string

    constructor(file: string) {
      this.file = file
    }

    prepare(sql: string) {
      return {
        all: (...args: unknown[]) => {
          const minTime = Number(args[0])
          const pattern = String(args[1]).replace(/^%|%$/g, '')
          const cwd = typeof args[2] === 'string' ? args[2] : null
          const rows = rowsByDbPath.get(this.file) ?? []
          return rows
            .filter((row) => row.time_created >= minTime)
            .filter((row) => row.data.includes(pattern))
            .filter((row) => !sql.includes('s.directory = ?') || row.directory === cwd)
            .map(({ directory: _directory, ...row }) => row)
        }
      }
    }

    close() {
      /* noop */
    }
  }

  return { default: MockDatabase }
})

const { listOpenCodeDbCandidates, readLatestOpenCodeRemoteImReply } = await import(
  './opencodeTranscript.js'
)

function textPart(text: string, timeCreated: number): string {
  return JSON.stringify({
    type: 'text',
    text,
    time: { start: timeCreated, end: timeCreated + 100 }
  })
}

describe('OpenCode transcript remote IM replies', () => {
  it('discovers OpenCode sqlite databases from the data dir', () => {
    const dir = mkdtempSync(join(tmpdir(), 'multi-ai-code-opencode-db-'))
    const dbPath = join(dir, 'opencode-dev.db')
    writeFileSync(dbPath, '')

    expect(listOpenCodeDbCandidates({ dataDir: dir })).toEqual([dbPath])
  })

  it('reads the latest assistant reply by cwd and reply id', () => {
    const dir = mkdtempSync(join(tmpdir(), 'multi-ai-code-opencode-db-'))
    const dbPath = join(dir, 'opencode-dev.db')
    writeFileSync(dbPath, '')
    const cwd = '/Users/me/work/repo'
    rowsByDbPath.set(dbPath, [
      {
        session_id: 'ses-other',
        directory: '/Users/me/other/repo',
        time_created: 2400,
        data: textPart(
          [
            '<remote-im-reply id="rim-current">',
            'wrong cwd',
            '</remote-im-reply id="rim-current">'
          ].join('\n'),
          2400
        )
      },
      {
        session_id: 'ses-current',
        directory: cwd,
        time_created: 2300,
        data: textPart(
          [
            '<remote-im-reply id="rim-current">',
            'OpenCode reply for IM.',
            '</remote-im-reply id="rim-current">'
          ].join('\n'),
          2300
        )
      },
      {
        session_id: 'ses-current',
        directory: cwd,
        time_created: 2500,
        data: textPart(
          [
            '<remote-im-reply id="rim-other">',
            'newer but wrong id',
            '</remote-im-reply id="rim-other">'
          ].join('\n'),
          2500
        )
      }
    ])

    expect(
      readLatestOpenCodeRemoteImReply({
        cwd,
        sinceMs: 2000,
        replyId: 'rim-current',
        dbPaths: [dbPath]
      })
    ).toBe('OpenCode reply for IM.')
  })

  it('uses reply id fallback when cwd storage differs from the current platform path', () => {
    const dir = mkdtempSync(join(tmpdir(), 'multi-ai-code-opencode-db-'))
    const dbPath = join(dir, 'opencode-dev.db')
    writeFileSync(dbPath, '')
    rowsByDbPath.set(dbPath, [
      {
        session_id: 'ses-current',
        directory: '/stored/path',
        time_created: 2300,
        data: textPart(
          [
            '<remote-im-reply id="rim-current">',
            'fallback reply',
            '</remote-im-reply id="rim-current">'
          ].join('\n'),
          2300
        )
      }
    ])

    expect(
      readLatestOpenCodeRemoteImReply({
        cwd: '/different/path',
        sinceMs: 2000,
        replyId: 'rim-current',
        dbPaths: [dbPath]
      })
    ).toBe('fallback reply')
  })
})
