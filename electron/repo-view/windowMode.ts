export type WindowMode =
  | { kind: 'main' }
  | { kind: 'repo-view'; projectId: string }

export function parseWindowModeSearch(search: string): WindowMode {
  const params = new URLSearchParams(search.startsWith('?') ? search.slice(1) : search)
  if (params.get('window') !== 'repo-view') return { kind: 'main' }
  const projectId = params.get('projectId')?.trim()
  if (!projectId) return { kind: 'main' }
  return { kind: 'repo-view', projectId }
}

export function buildRepoViewSearch(projectId: string): string {
  const params = new URLSearchParams()
  params.set('window', 'repo-view')
  params.set('projectId', projectId)
  return `?${params.toString()}`
}
