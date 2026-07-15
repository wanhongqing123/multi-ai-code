#!/usr/bin/env node
import { readFile } from 'node:fs/promises'
import { createRequire } from 'node:module'
import { homedir } from 'node:os'
import { join } from 'node:path'

const require = createRequire(import.meta.url)
const iconv = require('iconv-lite')

const HELP = `imcli help

Usage:
  imcli help
  imcli whoami [--project <projectId>]
  imcli contacts [--project <projectId>]
  imcli history [--peer <user>] [--limit <n>] [--project <projectId>]
  imcli last [--peer <user>] [--project <projectId>]
  imcli send <user> <text> [--project <projectId>]
  imcli send-image <user> <imagePath> [--project <projectId>]
  imcli send-file <user> <filePath> [--project <projectId>]
  imcli forward <user> --message-id <id> [--project <projectId>]
  imcli broadcast <user1,user2> <text> [--project <projectId>]

Command details:
  imcli whoami
    Print the desktop IM account, SDKAppID, and connection state.
    Use this first when you are unsure whether Remote IM is logged in.
    Output fields are UserID, SDKAppID, and Status.

  imcli contacts
    List configured peer user IDs that the current desktop account can message.
    The output is one user ID per line so AICLI can parse it safely.
    Use one of these user IDs as the <user> argument for send, send-image, send-file, or forward.

  imcli history
    Print recent IM messages. Use --peer <user> to narrow to one conversation.
    Message lines include the local store id. Use that id with imcli forward.
    Default limit is 20. Use --limit <n> for a larger or smaller window.
    Output format: #<id> <role>/<direction> <from> -> <to>: <content>
    Newlines inside message content are escaped as \\n to keep one message per line.

  imcli last
    Print only the last AICLI reply from history when available.
    This is useful when a task wants to inspect the previous IM answer.
    Use --peer <user> to avoid reading the wrong conversation.
    If no AICLI reply exists, it falls back to the last message in the selected history window.

  imcli send <user> <text>
    Send a plain text IM message to one user.
    Use quotes around multi-line or spaced text in shell commands.
    Use this for short text answers. For long Markdown/HTML reports, prefer send-file.
    The command repairs common GBK/UTF-8 mojibake before sending.

  imcli send-image <user> <imagePath>
    Send a local image file to one user.
    Use this for screenshots and visual results.
    The file path may contain spaces when quoted by the shell.
    Supported extensions: png, jpg, jpeg, gif, webp.

  imcli send-file <user> <filePath>
    Send a local Markdown or HTML document to one user.
    Use this when the answer is better as a report, checklist, design note, or rendered preview.
    The receiver can tap the file card in iOS, Android, or Desktop IM to preview it.
    Supported extensions: md, markdown, html, htm.

  imcli forward <user> --message-id <id>
    Forward the stored text content of one local history message to another user.
    It does not re-send image or file attachments.
    First run imcli history to find the numeric #<id>.
    This is for forwarding text already stored in the local Remote IM history.

  imcli broadcast <user1,user2> <text>
    Send the same plain text message to multiple comma-separated users.
    This is text-only. Use send-image or send-file separately for attachments.
    The command prints one sent line per target.

Requirements:
  Multi-AI Code desktop must be running with Remote IM connected.
  Provide a project with --project <projectId> or MULTI_AI_CODE_PROJECT_ID.
  AICLI sessions launched by Multi-AI Code usually already have the project env set.

File notes:
  Use send-image for png/jpg/jpeg/gif/webp image files.
  send-image accepts local png, jpg, jpeg, gif, and webp files up to 20MB.
  Markdown and HTML files should be sent with send-file, not send.
  send-file accepts local md, markdown, html, and htm files up to 5MB.
  forward sends the source message text only; it does not re-send image or file attachments.
  If a file is too large or the extension is unsupported, imcli fails before sending.

Examples:
  imcli whoami --project project-1
  imcli contacts --project project-1
  imcli history --peer phone-user --limit 20 --project project-1
  imcli send phone-user "build passed" --project project-1
  imcli send-image phone-user C:\\temp\\screenshot.png --project project-1
  imcli send-file phone-user ./report.md --project project-1
  imcli broadcast phone-user,desktop-b "ready" --project project-1

Notes:
  imcli talks to the running Multi-AI Code app through a local authenticated bridge.
  Use this from AICLI when you need to query or operate Remote IM.
`

function rootDir() {
  return process.env.MULTI_AI_CODE_ROOT_DIR || process.env.MULTI_AI_ROOT || join(homedir(), 'MultiAICode')
}

async function loadBridge() {
  const url = process.env.MULTI_AI_CODE_IMCLI_URL
  const token = process.env.MULTI_AI_CODE_IMCLI_TOKEN
  if (url && token) return { url, token }
  const raw = JSON.parse(await readFile(join(rootDir(), 'imcli-bridge.json'), 'utf8'))
  if (typeof raw.url === 'string' && typeof raw.token === 'string') {
    return { url: raw.url, token: raw.token }
  }
  throw new Error('invalid imcli bridge file')
}

function requireProjectId(args) {
  const projectIndex = args.indexOf('--project')
  if (projectIndex >= 0) {
    const value = args[projectIndex + 1]?.trim()
    if (value) return value
  }
  const value = process.env.MULTI_AI_CODE_PROJECT_ID?.trim()
  if (value) return value
  throw new Error('project id is required; set MULTI_AI_CODE_PROJECT_ID or pass --project <id>')
}

function getFlag(args, name) {
  const index = args.indexOf(name)
  if (index < 0) return null
  return args[index + 1] ?? null
}

function withoutProjectArgs(args) {
  const index = args.indexOf('--project')
  if (index < 0) return args
  return args.filter((_item, itemIndex) => itemIndex !== index && itemIndex !== index + 1)
}

const MOJIBAKE_MARKERS = [
  '銆',
  '锛',
  '鈥',
  '涓',
  '鍙',
  '姣',
  '鏍',
  '鍒',
  '淇',
  '鏂',
  '鐗',
  '绐',
  '洜',
  '瑰',
  '堟',
  '忓',
  '楀'
]

function countOccurrences(text, needle) {
  if (!needle) return 0
  let count = 0
  let index = text.indexOf(needle)
  while (index >= 0) {
    count += 1
    index = text.indexOf(needle, index + needle.length)
  }
  return count
}

function mojibakeScore(text) {
  let score = 0
  for (const marker of MOJIBAKE_MARKERS) score += countOccurrences(text, marker)
  score += countOccurrences(text, '\uFFFD') * 3
  score += /銆[?�]/.test(text) ? 3 : 0
  return score
}

function encodeGb18030(text) {
  return Buffer.concat([...text].map((char) => iconv.encode(char, 'gb18030')))
}

function repairLikelyUtf8DecodedAsGbk(text) {
  const originalScore = mojibakeScore(text)
  if (originalScore < 4) return text

  const repaired = encodeGb18030(text)
    .toString('utf8')
    .replace(/\uFFFD\?/g, '】')
  return mojibakeScore(repaired) < originalScore ? repaired : text
}

function normalizeOutgoingText(text) {
  return repairLikelyUtf8DecodedAsGbk(text.trim())
}

async function requestJson(method, path, body) {
  const bridge = await loadBridge()
  const response = await fetch(`${bridge.url}${path}`, {
    method,
    headers: {
      authorization: `Bearer ${bridge.token}`,
      ...(body ? { 'content-type': 'application/json' } : {})
    },
    body: body ? JSON.stringify(body) : undefined
  })
  const payload = await response.json()
  if (!payload.ok) {
    throw new Error(payload.error || `imcli request failed with HTTP ${response.status}`)
  }
  return payload.value
}

function printMessages(messages) {
  if (!messages.length) {
    console.log('no messages')
    return
  }
  for (const message of messages) {
    const from = message.fromUserId || '-'
    const to = message.toUserId || '-'
    const content = String(message.content || '').replace(/\n/g, '\\n')
    console.log(`#${message.id} ${message.role}/${message.direction} ${from} -> ${to}: ${content}`)
  }
}

async function main(argv) {
  const [rawCommand = 'help', ...rawArgs] = argv
  const command = rawCommand === '--help' || rawCommand === '-h' ? 'help' : rawCommand
  const args = withoutProjectArgs(rawArgs)

  if (command === 'help') {
    console.log(HELP)
    return
  }

  const projectId = requireProjectId(rawArgs)

  if (command === 'whoami') {
    const value = await requestJson('GET', `/whoami?projectId=${encodeURIComponent(projectId)}`)
    console.log(`UserID: ${value.userId || '-'}`)
    console.log(`SDKAppID: ${value.sdkAppId || '-'}`)
    console.log(`Status: ${value.status}${value.statusDetail ? ` (${value.statusDetail})` : ''}`)
    return
  }

  if (command === 'contacts') {
    const value = await requestJson('GET', `/contacts?projectId=${encodeURIComponent(projectId)}`)
    if (!value.contacts.length) {
      console.log('no contacts')
      return
    }
    for (const contact of value.contacts) console.log(contact.userId)
    return
  }

  if (command === 'history' || command === 'last') {
    const peer = getFlag(args, '--peer')
    const limit = getFlag(args, '--limit') || (command === 'last' ? '50' : '20')
    const query = new URLSearchParams({ projectId, limit })
    if (peer) query.set('peer', peer)
    const value = await requestJson('GET', `/history?${query.toString()}`)
    if (command === 'history') {
      printMessages(value.messages)
      return
    }
    const last =
      [...value.messages].reverse().find((message) => message.role === 'aicli') ??
      value.messages[value.messages.length - 1]
    if (!last) {
      console.log('no messages')
      return
    }
    console.log(last.content)
    return
  }

  if (command === 'send') {
    const [toUserId, ...textParts] = args
    const text = normalizeOutgoingText(textParts.join(' '))
    if (!toUserId || !text) throw new Error('usage: imcli send <user> <text>')
    const value = await requestJson('POST', '/send', { projectId, toUserId, text })
    console.log(`sent to ${value.toUserId}`)
    return
  }

  if (command === 'send-image') {
    const [toUserId, ...pathParts] = args
    const localPath = pathParts.join(' ').trim()
    if (!toUserId || !localPath) throw new Error('usage: imcli send-image <user> <imagePath>')
    const value = await requestJson('POST', '/send-image', { projectId, toUserId, localPath })
    console.log(`sent image to ${value.toUserId}`)
    return
  }

  if (command === 'send-file') {
    const [toUserId, ...pathParts] = args
    const localPath = pathParts.join(' ').trim()
    if (!toUserId || !localPath) throw new Error('usage: imcli send-file <user> <filePath>')
    const value = await requestJson('POST', '/send-file', { projectId, toUserId, localPath })
    console.log(`sent file to ${value.toUserId}`)
    return
  }

  if (command === 'broadcast') {
    const [targets, ...textParts] = args
    const text = normalizeOutgoingText(textParts.join(' '))
    if (!targets || !text) throw new Error('usage: imcli broadcast <user1,user2> <text>')
    for (const toUserId of targets.split(',').map((item) => item.trim()).filter(Boolean)) {
      const value = await requestJson('POST', '/send', { projectId, toUserId, text })
      console.log(`sent to ${value.toUserId}`)
    }
    return
  }

  if (command === 'forward') {
    const [toUserId] = args
    const messageId = Number(getFlag(args, '--message-id'))
    if (!toUserId || !Number.isInteger(messageId)) {
      throw new Error('usage: imcli forward <user> --message-id <id>')
    }
    const value = await requestJson('GET', `/history?${new URLSearchParams({ projectId, limit: '200' }).toString()}`)
    const message = value.messages.find((item) => Number(item.id) === messageId)
    if (!message) throw new Error(`message not found: ${messageId}`)
    const sent = await requestJson('POST', '/send', {
      projectId,
      toUserId,
      text: String(message.content || '')
    })
    console.log(`forwarded #${messageId} to ${sent.toUserId}`)
    return
  }

  throw new Error(`unknown command: ${command}`)
}

main(process.argv.slice(2)).catch((error) => {
  console.error(`imcli: ${error instanceof Error ? error.message : String(error)}`)
  process.exitCode = 1
})
