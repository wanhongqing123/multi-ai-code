import { describe, expect, it } from 'vitest'
import {
  clearAnnotationVisualState,
  removeAnnotationVisualState,
  startEditingAnnotationState,
  trackNewAnnotationState,
  type AnnotationVisualState
} from './annotationVisualState'

describe('trackNewAnnotationState', () => {
  it('focuses the new annotation and marks it as recently added', () => {
    expect(trackNewAnnotationState('ann_1')).toEqual({
      activeAnnotationId: 'ann_1',
      recentlyAddedAnnotationId: 'ann_1'
    })
  })
})

describe('startEditingAnnotationState', () => {
  it('moves focus to the edited annotation and clears the recent marker', () => {
    const prev: AnnotationVisualState = {
      activeAnnotationId: 'ann_1',
      recentlyAddedAnnotationId: 'ann_1'
    }
    expect(startEditingAnnotationState(prev, 'ann_2')).toEqual({
      activeAnnotationId: 'ann_2',
      recentlyAddedAnnotationId: null
    })
  })
})

describe('removeAnnotationVisualState', () => {
  it('clears the active and recent ids when the removed annotation matches both', () => {
    const prev: AnnotationVisualState = {
      activeAnnotationId: 'ann_2',
      recentlyAddedAnnotationId: 'ann_2'
    }
    expect(removeAnnotationVisualState(prev, 'ann_2')).toEqual({
      activeAnnotationId: null,
      recentlyAddedAnnotationId: null
    })
  })

  it('keeps unrelated state intact when a different annotation is removed', () => {
    const prev: AnnotationVisualState = {
      activeAnnotationId: 'ann_2',
      recentlyAddedAnnotationId: 'ann_3'
    }
    expect(removeAnnotationVisualState(prev, 'ann_1')).toEqual(prev)
  })
})

describe('clearAnnotationVisualState', () => {
  it('returns the idle state', () => {
    expect(clearAnnotationVisualState()).toEqual({
      activeAnnotationId: null,
      recentlyAddedAnnotationId: null
    })
  })
})
