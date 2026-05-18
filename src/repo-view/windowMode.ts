export type RendererWindowMode =
  | { kind: 'main' }
  | { kind: 'repo-view'; projectId: string }
  | { kind: 'screenshot-overlay'; sessionToken: string }
  | { kind: 'screenshot-editor'; sessionToken: string }

export function parseRendererWindowModeSearch(search: string): RendererWindowMode {
  const params = new URLSearchParams(search.startsWith('?') ? search.slice(1) : search)
  const windowKind = params.get('window')
  if (windowKind === 'screenshot-overlay') {
    const sessionToken = params.get('token')?.trim()
    if (!sessionToken) return { kind: 'main' }
    return { kind: 'screenshot-overlay', sessionToken }
  }
  if (windowKind === 'screenshot-editor') {
    const sessionToken = params.get('token')?.trim()
    if (!sessionToken) return { kind: 'main' }
    return { kind: 'screenshot-editor', sessionToken }
  }
  if (windowKind !== 'repo-view') return { kind: 'main' }
  const projectId = params.get('projectId')?.trim()
  if (!projectId) return { kind: 'main' }
  return { kind: 'repo-view', projectId }
}
