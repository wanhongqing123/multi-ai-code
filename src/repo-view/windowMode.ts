export type RendererWindowMode =
  | { kind: 'main' }
  | { kind: 'repo-view'; projectId: string }

export function parseRendererWindowModeSearch(search: string): RendererWindowMode {
  const params = new URLSearchParams(search.startsWith('?') ? search.slice(1) : search)
  if (params.get('window') !== 'repo-view') return { kind: 'main' }
  const projectId = params.get('projectId')?.trim()
  if (!projectId) return { kind: 'main' }
  return { kind: 'repo-view', projectId }
}
