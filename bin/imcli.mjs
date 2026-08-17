#!/usr/bin/env node
import { readFile } from 'node:fs/promises'
import { homedir } from 'node:os'
import { join } from 'node:path'

const HELP = `imcli help

Usage:
  imcli help
  imcli whoami [--project <projectId>]
  imcli contacts [--project <projectId>]
  imcli history [--peer <user>] [--limit <n>] [--project <projectId>]
  imcli last [--peer <user>] [--project <projectId>]
  imcli send <user> --text-b64 <base64> [--project <projectId>]
  imcli send-image <user> <imagePath> [--project <projectId>]
  imcli send-file <user> <filePath> [--project <projectId>]
  imcli forward <user> --message-id <id> [--project <projectId>]
  imcli broadcast <user1,user2> --text-b64 <base64> [--project <projectId>]

Message text is always Base64 — READ THIS FIRST:
  Encode the UTF-8 message body as standard Base64 and pass it to --text-b64:
      node -e "process.stdout.write(Buffer.from('标题\\n第二行','utf8').toString('base64'))"
      imcli send phone-user --text-b64 5qCH6aKYCuesrOS6jOihjA==
  Base64 is the only channel with no failure mode: it is a single line (nothing can
  truncate it), pure ASCII (no code page can degrade it), and contains no shell
  metacharacters (no quoting, %, backtick or backslash surprises). Decoding is exact,
  so a literal "C:\\new" and a real newline can never be confused.
  Plain text arguments, stdin pipes and \\n escapes were all removed: each of them
  silently corrupted messages in practice (cmd.exe truncates at the first newline,
  PowerShell 5.1 encodes pipes as ASCII and turns non-ASCII into '?', and \\n
  expansion mangled Windows paths like C:\\new).
  Attachments do not use this flag — send-image and send-file take a path directly.

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

  imcli send <user> --text-b64 <base64>
    Send a plain text IM message to one user.
    The body is standard Base64 of the UTF-8 text. Any content is safe this way:
    multi-line, Chinese, quotes, backslashes, percent signs.
      imcli send phone-user --text-b64 5qCH6aKYCuesrOS6jOihjA==
    imcli rejects input that is not valid Base64 instead of sending a damaged
    message, so a truncated payload fails loudly rather than arriving mangled.
    Use send-file when the receiver should get an attachment card instead of text.

  imcli send-image <user> <imagePath>
    Send a local image file to one user.
    Use this for screenshots and visual results.
    The file path may contain spaces when quoted by the shell.
    Supported extensions: png, jpg, jpeg, gif, webp.

  imcli send-file <user> <filePath>
    Send a local file of any type to one user (up to 100MB).
    Markdown and HTML files render as documents: the receiver can tap the file card
    in iOS, Android, or Desktop IM to preview it.
    Any other file type (zip, pdf, txt, logs, binaries, ...) shows a file card that
    the receiver taps to save locally.

  imcli forward <user> --message-id <id>
    Forward the stored text content of one local history message to another user.
    It does not re-send image or file attachments.
    First run imcli history to find the numeric #<id>.
    This is for forwarding text already stored in the local Remote IM history.

  imcli broadcast <user1,user2> --text-b64 <base64>
    Send the same plain text message to multiple comma-separated users.
    Body encoding is identical to imcli send: standard Base64 of the UTF-8 text.
    This is text-only. Use send-image or send-file separately for attachments.
    The command prints one sent line per target.

Requirements:
  Multi-AI Code desktop must be running with Remote IM connected.
  Provide a project with --project <projectId> or MULTI_AI_CODE_PROJECT_ID.
  AICLI sessions launched by Multi-AI Code usually already have the project env set.

File notes:
  Use send-image for png/jpg/jpeg/gif/webp image files (renders as an inline picture).
  send-image accepts local png, jpg, jpeg, gif, and webp files up to 20MB.
  Markdown and HTML files should be sent with send-file, not send.
  send-file accepts any regular file up to 100MB; md/markdown/html/htm render with
  tap-to-preview on receivers, other types show a save-to-local file card.
  forward sends the source message text only; it does not re-send image or file attachments.
  If a file is too large, imcli fails before sending.

Examples:
  imcli whoami --project project-1
  imcli contacts --project project-1
  imcli history --peer phone-user --limit 20 --project project-1
  imcli send phone-user --text-b64 YnVpbGQgcGFzc2Vk --project project-1
  imcli send-image phone-user C:\\temp\\screenshot.png --project project-1
  imcli send-file phone-user ./report.md --project project-1
  imcli broadcast phone-user,desktop-b --text-b64 cmVhZHk= --project project-1

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

function withoutFlagPair(args, name) {
  const index = args.indexOf(name)
  if (index < 0) return args
  return args.filter((_item, itemIndex) => itemIndex !== index && itemIndex !== index + 1)
}

function withoutProjectArgs(args) {
  return withoutFlagPair(withoutFlagPair(args, '--project'), '--text-b64')
}

const TEXT_B64_FLAG = '--text-b64'

// Base64 是 send / broadcast 唯一的正文通道，因为只有它没有失败模式：
//   单行             —— cmd.exe 逐行解析，无从截断
//   纯 ASCII         —— 任何代码页都能无损表示，不会退化成 '?'
//   无 shell 元字符  —— 引号、%、反引号、反斜杠都不会出现
//   解码精确         —— 不靠任何猜测，"C:\new" 与真实换行天然可分
// 此前三条路（纯参数 / stdin 管道 / \n 字面量展开）都在靠 shell 行为或猜测还原
// 原文，且都实测会静默改坏正文：cmd 在换行处截断；PowerShell 5.1 按 ASCII 编码
// 管道把中文压成 '?'；\n 展开会把 C:\new 拆成换行。全部移除，不留半安全通道。
function decodeOutgoingText(rawArgs, usage) {
  const encoded = getFlag(rawArgs, TEXT_B64_FLAG)?.trim()
  if (!encoded) throw new Error(usage)
  if (!/^[A-Za-z0-9+/]+={0,2}$/.test(encoded)) {
    throw new Error(`${TEXT_B64_FLAG} must be standard Base64 (A-Z a-z 0-9 + / =)`)
  }
  const buffer = Buffer.from(encoded, 'base64')
  // Node 解码 base64 时会跳过非法字符而不报错；往回编一次才能证明拿到的是完整
  // Base64，而不是被 shell 截断过的半截。
  if (buffer.toString('base64').replace(/=+$/, '') !== encoded.replace(/=+$/, '')) {
    throw new Error(`${TEXT_B64_FLAG} is not valid Base64 (payload does not round-trip)`)
  }
  const text = buffer.toString('utf8').trim()
  if (!text) throw new Error(`${TEXT_B64_FLAG} decoded to an empty message`)
  return text
}

/**
 * 给 imcli 发出的正文加来源标注。
 *
 * 加在这里而不是让调用方自己拼：--text-b64 是调用方在执行 imcli 之前就编码
 * 好的，指望每个调用方都记得加，漏一次就没有。放在 imcli 内部才能保证"凡是
 * 走 imcli 发出去的就一定带上"。
 *
 * 只用于 send / broadcast —— 这两条的正文是调用方当场写的，标注来源不会引起
 * 误解。forward 复制的是别人（或以前的自己）写的原话，缀上这句会让收件人
 * 以为它是原文的一部分；send-image / send-file 没有正文。
 */
const IMCLI_ORIGIN_SUFFIX = '此消息来自 imcli'

export function withImcliOriginSuffix(text) {
  const body = String(text ?? '').trimEnd()
  // 已经带了就不再叠加：转发自己发过的内容、或调用方手工加过时都会撞上。
  if (body.endsWith(IMCLI_ORIGIN_SUFFIX)) return body
  // 空行隔开，避免和正文最后一句粘成一行。
  return `${body}

${IMCLI_ORIGIN_SUFFIX}`
}

async function requestJson(method, path, body) {
  const bridge = await loadBridge()
  const sessionId = process.env.MULTI_AI_CODE_SESSION_ID?.trim()
  const response = await fetch(`${bridge.url}${path}`, {
    method,
    headers: {
      authorization: `Bearer ${bridge.token}`,
      ...(sessionId ? { 'x-multi-ai-code-session-id': sessionId } : {}),
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
    const [toUserId] = args
    const usage = `usage: imcli send <user> ${TEXT_B64_FLAG} <base64>`
    if (!toUserId) throw new Error(usage)
    const text = withImcliOriginSuffix(decodeOutgoingText(rawArgs, usage))
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
    const [targets] = args
    const usage = `usage: imcli broadcast <user1,user2> ${TEXT_B64_FLAG} <base64>`
    if (!targets) throw new Error(usage)
    const text = withImcliOriginSuffix(decodeOutgoingText(rawArgs, usage))
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
