import { cacheRemoteImImage, type RemoteImImageFetch } from './imageCache.js'
import type { RemoteImImageAttachment, RemoteImMessage } from './types.js'

interface RetryImageDependencies {
  rootDir: string
  fetchImpl: RemoteImImageFetch
  listById: (id: number) => RemoteImMessage | null
  restore: (projectId: string, id: number, url: string, attachment: RemoteImImageAttachment) => RemoteImMessage | null
  isAccountCurrent: () => boolean
}

// 只从当前账号记录读取地址；下载完成后重新核对原记录，且绝不重新投递消息。
export async function retryRemoteImImage(
  projectId: string, messageId: number, deps: RetryImageDependencies
): Promise<void> {
  if (!projectId || !Number.isSafeInteger(messageId) || messageId <= 0) throw new Error('invalid input')
  const message = deps.listById(messageId)
  const attachment = message?.attachment
  if (
    !message || message.projectId !== projectId || message.direction !== 'incoming' ||
    message.status !== 'failed' || message.kind !== 'image' || attachment?.type !== 'image' ||
    attachment.localPath || !attachment.remoteUrl
  ) throw new Error('image cannot be retried')
  const url = new URL(attachment.remoteUrl)
  if (url.protocol !== 'https:' && url.protocol !== 'http:') throw new Error('invalid image URL')
  const cached = await cacheRemoteImImage({
    rootDir: deps.rootDir,
    projectId,
    remoteUrl: attachment.remoteUrl,
    remoteMessageId: message.remoteMessageId,
    fileName: attachment.fileName,
    mimeType: attachment.mimeType,
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
  if (!restored) throw new Error('image message changed')
}
