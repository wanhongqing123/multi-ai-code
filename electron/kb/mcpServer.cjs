#!/usr/bin/env node
/**
 * Multi-AI Code project-knowledge-base MCP server.
 *
 * Spawned by Claude Code via the temporary mcp-config we inject when the
 * main session starts (see electron/kb/mcpConfig.ts). Reads the repo path
 * and the platform DB path from CLI args / env, opens the SQLite DB
 * read-only, and exposes three tools over stdio JSON-RPC 2.0:
 *
 *   query_kb(query, limit?)  → top matches by FTS5 / LIKE
 *   list_topics()            → all topics for this repo
 *   get_topic(topic)         → all entries matching that topic
 *
 * Constraints:
 *   - Pure Node (no Electron). Avoids native better-sqlite3 binding by
 *     using a tiny SQL bridge over the same DB file via @vscode/sqlite3?
 *     For v1 we try better-sqlite3 first (most users running Electron
 *     already have a matching binary cached on disk under node_modules),
 *     then fall back to a "feature unavailable" error if it fails to load.
 *
 * MCP protocol notes:
 *   - JSON-RPC 2.0 over LSP-style framed messages on stdio
 *   - Each message: "Content-Length: N\r\n\r\n<json>"
 *   - We support: initialize, tools/list, tools/call
 */

const path = require('path')
const fs = require('fs')

// ---------- arg parsing ----------

const ARGS = (() => {
  const out = {}
  for (let i = 2; i < process.argv.length; i++) {
    const a = process.argv[i]
    if (a === '--repo' || a === '--db' || a === '--limit-default') {
      out[a.slice(2)] = process.argv[++i]
    }
  }
  return out
})()

// --repo is accepted for backward-compatibility but ignored: each repo
// now has its own kb.db file, passed via --db.
const DB_PATH = ARGS.db || process.env.MAC_KB_DB_PATH || ''
const DEFAULT_LIMIT = Number.parseInt(ARGS['limit-default'] || '10', 10) || 10

// ---------- sqlite loader ----------

let db = null
let dbError = null
try {
  // eslint-disable-next-line @typescript-eslint/no-var-requires
  const Database = require('better-sqlite3')
  if (DB_PATH && fs.existsSync(DB_PATH)) {
    db = new Database(DB_PATH, { readonly: true, fileMustExist: true })
  } else if (DB_PATH) {
    dbError = `DB file not found: ${DB_PATH}`
  } else {
    dbError = 'DB_PATH was not provided'
  }
} catch (err) {
  dbError = `failed to open SQLite: ${err && err.message ? err.message : String(err)}`
}

// ---------- KB query helpers ----------

function escapeFtsQuery(q) {
  return String(q || '')
    .split(/\s+/)
    .map((t) => t.replace(/"/g, ''))
    .filter((t) => /\w|[一-鿿]/.test(t))
    .map((t) => `"${t}"`)
    .join(' ')
}

function queryKb(query, limit) {
  if (!db) return { error: dbError || 'db not loaded' }
  const lim = Math.max(1, Math.min(50, Number(limit) || DEFAULT_LIMIT))
  const trimmed = String(query || '').trim()
  if (!trimmed) return { entries: [] }
  try {
    const rows = db
      .prepare(
        `SELECT e.id, e.topic, e.summary, e.evidence, e.importance, e.tier,
                e.access_count, e.last_accessed_at, e.updated_at, bm25(kb_fts) AS bm
           FROM kb_fts
           JOIN kb_entries e ON e.id = kb_fts.rowid
          WHERE kb_fts MATCH ?
          ORDER BY bm ASC
          LIMIT ?`
      )
      .all(escapeFtsQuery(trimmed), lim)
    return { entries: rows.map(normalizeEntry) }
  } catch (err) {
    // FTS5 syntax errors fall through to LIKE
    try {
      const like = `%${trimmed.replace(/[%_]/g, (m) => '\\' + m)}%`
      const rows = db
        .prepare(
          `SELECT id, topic, summary, evidence, importance, tier,
                  access_count, last_accessed_at, updated_at
             FROM kb_entries
            WHERE topic LIKE ? ESCAPE '\\' OR summary LIKE ? ESCAPE '\\'
            ORDER BY updated_at DESC
            LIMIT ?`
        )
        .all(like, like, lim)
      return { entries: rows.map(normalizeEntry) }
    } catch (err2) {
      return {
        error: `query failed: ${err && err.message ? err.message : err}`
      }
    }
  }
}

function listTopics() {
  if (!db) return { error: dbError || 'db not loaded' }
  try {
    const rows = db
      .prepare(`SELECT DISTINCT topic FROM kb_entries ORDER BY topic ASC`)
      .all()
    return { topics: rows.map((r) => r.topic) }
  } catch (err) {
    return { error: err && err.message ? err.message : String(err) }
  }
}

function getTopic(topic) {
  if (!db) return { error: dbError || 'db not loaded' }
  if (!topic) return { entries: [] }
  try {
    const rows = db
      .prepare(
        `SELECT id, topic, summary, evidence, importance, tier,
                access_count, last_accessed_at, updated_at
           FROM kb_entries
          WHERE topic = ?
          ORDER BY updated_at DESC`
      )
      .all(String(topic))
    return { entries: rows.map(normalizeEntry) }
  } catch (err) {
    return { error: err && err.message ? err.message : String(err) }
  }
}

function normalizeEntry(row) {
  let evidence = {}
  if (row.evidence) {
    try {
      evidence = JSON.parse(row.evidence)
    } catch {
      /* ignore */
    }
  }
  return {
    id: row.id,
    topic: row.topic,
    summary: row.summary,
    evidence,
    importance: row.importance,
    tier: row.tier,
    accessCount: row.access_count,
    lastAccessedAt: row.last_accessed_at,
    updatedAt: row.updated_at
  }
}

// ---------- MCP JSON-RPC ----------

const TOOLS = [
  {
    name: 'query_kb',
    description:
      "Full-text search over the project's local knowledge base. Returns the top matches with topic + summary.",
    inputSchema: {
      type: 'object',
      properties: {
        query: { type: 'string', description: 'Keyword query.' },
        limit: { type: 'number', description: 'Max results (default 10).' }
      },
      required: ['query']
    }
  },
  {
    name: 'list_topics',
    description: 'List every topic name stored for this project.',
    inputSchema: { type: 'object', properties: {} }
  },
  {
    name: 'get_topic',
    description: 'Return every KB entry whose topic exactly matches the argument.',
    inputSchema: {
      type: 'object',
      properties: {
        topic: { type: 'string', description: 'Topic name to fetch.' }
      },
      required: ['topic']
    }
  }
]

function jsonReply(id, result, error) {
  const msg = error
    ? { jsonrpc: '2.0', id, error }
    : { jsonrpc: '2.0', id, result }
  const body = JSON.stringify(msg)
  const header = `Content-Length: ${Buffer.byteLength(body, 'utf8')}\r\n\r\n`
  process.stdout.write(header + body)
}

function handleRequest(msg) {
  const { id, method, params } = msg
  if (method === 'initialize') {
    jsonReply(id, {
      protocolVersion: '2024-11-05',
      capabilities: { tools: {} },
      serverInfo: { name: 'multi-ai-code-kb', version: '1.0.0' }
    })
    return
  }
  if (method === 'tools/list') {
    jsonReply(id, { tools: TOOLS })
    return
  }
  if (method === 'tools/call') {
    const name = params && params.name
    const args = (params && params.arguments) || {}
    let result
    if (name === 'query_kb') {
      result = queryKb(args.query, args.limit)
    } else if (name === 'list_topics') {
      result = listTopics()
    } else if (name === 'get_topic') {
      result = getTopic(args.topic)
    } else {
      jsonReply(id, null, { code: -32601, message: `unknown tool: ${name}` })
      return
    }
    jsonReply(id, {
      content: [{ type: 'text', text: JSON.stringify(result, null, 2) }]
    })
    return
  }
  // Unknown method.
  jsonReply(id, null, { code: -32601, message: `method not found: ${method}` })
}

// ---------- stdio framing ----------

let buffer = Buffer.alloc(0)
let expected = -1

process.stdin.on('data', (chunk) => {
  buffer = Buffer.concat([buffer, chunk])
  // Loop in case multiple messages arrived in one chunk.
  // eslint-disable-next-line no-constant-condition
  while (true) {
    if (expected < 0) {
      const headerEnd = buffer.indexOf('\r\n\r\n')
      if (headerEnd < 0) return
      const header = buffer.slice(0, headerEnd).toString('utf8')
      const m = header.match(/Content-Length:\s*(\d+)/i)
      if (!m) {
        // Malformed — discard and reset.
        buffer = buffer.slice(headerEnd + 4)
        continue
      }
      expected = Number(m[1])
      buffer = buffer.slice(headerEnd + 4)
    }
    if (buffer.length < expected) return
    const body = buffer.slice(0, expected).toString('utf8')
    buffer = buffer.slice(expected)
    expected = -1
    let msg
    try {
      msg = JSON.parse(body)
    } catch {
      continue
    }
    if (Array.isArray(msg)) {
      for (const m of msg) handleRequest(m)
    } else {
      handleRequest(msg)
    }
  }
})

process.stdin.on('end', () => {
  process.exit(0)
})

process.on('SIGINT', () => process.exit(0))
process.on('SIGTERM', () => process.exit(0))
