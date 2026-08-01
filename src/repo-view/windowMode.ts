export type RendererWindowMode =
  | { kind: 'main' }
  | { kind: 'repo-view'; projectId: string }

export function parseRendererWindowModeSearch(search: string): RendererWindowMode {
  const params = new URLSearchParams(search.startsWith('?') ? search.slice(1) : search)
  const windowKind = params.get('window')
  if (windowKind !== 'repo-view') return { kind: 'main' }
  const projectId = params.get('projectId')?.trim()
  if (!projectId) return { kind: 'main' }
  return { kind: 'repo-view', projectId }
}
