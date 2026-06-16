import type { RuntimeState } from '../../electron/preload'

export function selectVisibleRuntimeState(
  currentProjectId: string | null,
  runtimeState: RuntimeState,
  emptyRuntimeState: RuntimeState
): RuntimeState {
  if (currentProjectId !== null && runtimeState.projectId === currentProjectId) {
    return runtimeState
  }

  if (runtimeState.status === 'running') {
    return runtimeState
  }

  return emptyRuntimeState
}
