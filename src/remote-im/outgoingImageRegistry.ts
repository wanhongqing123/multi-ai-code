const outgoingImageFiles = new Map<string, File>()

function createFileToken(): string {
  if (typeof crypto !== 'undefined' && typeof crypto.randomUUID === 'function') {
    return crypto.randomUUID()
  }
  return `image-${Date.now()}-${Math.random().toString(36).slice(2)}`
}

export function registerRemoteImOutgoingImageFile(file: File): string {
  const token = createFileToken()
  outgoingImageFiles.set(token, file)
  return token
}

export function resolveRemoteImOutgoingImageFile(token: string): File | null {
  return outgoingImageFiles.get(token) ?? null
}

export function forgetRemoteImOutgoingImageFile(token: string): void {
  outgoingImageFiles.delete(token)
}
