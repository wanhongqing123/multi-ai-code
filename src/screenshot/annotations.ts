/**
 * Annotation data model for the screenshot editor. Each shape is a plain
 * value object so the editor's history (used for undo) can be a flat array.
 * Rendering and hit-test logic are kept separate (in AnnotationEditor.tsx).
 */

export type AnnotationColor =
  | '#ef4444' // red
  | '#f59e0b' // orange
  | '#10b981' // green
  | '#3b82f6' // blue
  | '#111827' // near-black

export const ANNOTATION_COLORS: AnnotationColor[] = [
  '#ef4444',
  '#f59e0b',
  '#10b981',
  '#3b82f6',
  '#111827'
]

export type AnnotationTool = 'rect' | 'arrow' | 'text' | 'mosaic'

export interface RectAnnotation {
  kind: 'rect'
  color: AnnotationColor
  x: number
  y: number
  w: number
  h: number
}

export interface ArrowAnnotation {
  kind: 'arrow'
  color: AnnotationColor
  /** Tail end (where the user started drawing). */
  fromX: number
  fromY: number
  /** Arrowhead end. */
  toX: number
  toY: number
}

export interface TextAnnotation {
  kind: 'text'
  color: AnnotationColor
  x: number
  y: number
  fontSize: number
  text: string
}

export interface MosaicAnnotation {
  kind: 'mosaic'
  x: number
  y: number
  w: number
  h: number
  /** Pixel block size used to render the blur effect. */
  block: number
}

export type Annotation =
  | RectAnnotation
  | ArrowAnnotation
  | TextAnnotation
  | MosaicAnnotation

export const DEFAULT_TEXT_FONT_SIZE = 18
export const DEFAULT_MOSAIC_BLOCK = 12
export const ANNOTATION_STROKE_WIDTH = 3

/** Apply a single new annotation to a history list, dropping any redo state. */
export function appendAnnotation(
  list: Annotation[],
  next: Annotation
): Annotation[] {
  return [...list, next]
}

/** Pop the last annotation; returns the same list if already empty. */
export function undoLast(list: Annotation[]): Annotation[] {
  if (list.length === 0) return list
  return list.slice(0, -1)
}
