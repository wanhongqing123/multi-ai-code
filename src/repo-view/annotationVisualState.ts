export interface AnnotationVisualState {
  activeAnnotationId: string | null
  recentlyAddedAnnotationId: string | null
}

export function clearAnnotationVisualState(): AnnotationVisualState {
  return {
    activeAnnotationId: null,
    recentlyAddedAnnotationId: null
  }
}

export function trackNewAnnotationState(
  annotationId: string
): AnnotationVisualState {
  return {
    activeAnnotationId: annotationId,
    recentlyAddedAnnotationId: annotationId
  }
}

export function startEditingAnnotationState(
  _prev: AnnotationVisualState,
  annotationId: string
): AnnotationVisualState {
  return {
    activeAnnotationId: annotationId,
    recentlyAddedAnnotationId: null
  }
}

export function removeAnnotationVisualState(
  prev: AnnotationVisualState,
  annotationId: string
): AnnotationVisualState {
  return {
    activeAnnotationId:
      prev.activeAnnotationId === annotationId ? null : prev.activeAnnotationId,
    recentlyAddedAnnotationId:
      prev.recentlyAddedAnnotationId === annotationId
        ? null
        : prev.recentlyAddedAnnotationId
  }
}
