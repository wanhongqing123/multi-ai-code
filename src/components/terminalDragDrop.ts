export interface DroppedPathLike {
  path?: string
}

export function buildDroppedFileInput<T extends DroppedPathLike>(
  files: Iterable<T> | ArrayLike<T>,
  resolvePath?: (file: T) => string
): string {
  return Array.from(files)
    .map((file) => {
      const resolved = resolvePath ? resolvePath(file) : ''
      return (resolved || file.path || '').trim()
    })
    .filter((path) => path.length > 0)
    .join(' ')
}
