import { cacheRemoteImFile, type RemoteImFileFetch } from './fileCache.js'
import type { RemoteImFileAttachment, RemoteImMessage } from './types.js'

interface RetryFileDependencies {
  rootDir: string
  fetchImpl: RemoteImFileFetch
  maxBytes: number
  listById: (id: number) => RemoteImMessage | null
  restore: (projectId: string, id: number, url: string, attachment: RemoteImFileAttachment) => RemoteImMessage | null
  isAccountCurrent: () => boolean
}

// 从当前账号的原消息读取地址，下载后再次核对账号和消息；不重新投递给 Agent。
export async function retryRemoteImFile(
  projectId: string, messageId: number, deps: RetryFileDependencies
): Promise<void> {
  if (!projectId || !Number.isSafeInteger(messageId) || messageId <= 0) throw new Error('invalid input')
  const message = deps.listById(messageId)
  const attachment = message?.attachment
  if (
    !message || message.projectId !== projectId || message.direction !== 'incoming' ||
    message.status !== 'failed' || message.kind !== 'file' || attachment?.type !== 'file' ||
    attachment.localPath || !attachment.remoteUrl
  ) throw new Error('file cannot be retried')
  const url = new URL(attachment.remoteUrl)
  if (url.protocol !== 'https:' && url.protocol !== 'http:') throw new Error('invalid file URL')
  const cached = await cacheRemoteImFile({
    rootDir: deps.rootDir,
    projectId,
    remoteUrl: attachment.remoteUrl,
    remoteMessageId: message.remoteMessageId,
    fileName: attachment.fileName,
    mimeType: attachment.mimeType,
    // 与首次接收使用同一文件大小限制，避免大文件仅在重试时被拒绝。
    maxBytes: deps.maxBytes,
    fetchImpl: deps.fetchImpl
  })
  if (!deps.isAccountCurrent()) throw new Error('account changed')
  const restored = deps.restore(projectId, messageId, attachment.remoteUrl, {
    ...attachment,
    localPath: cached.localPath,
    fileName: cached.fileName,
    mimeType: cached.mimeType,
    sizeBytes: cached.sizeBytes
  })
  if (!restored) throw new Error('file message changed')
}
