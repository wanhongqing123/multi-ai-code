import { promises as fs } from 'fs'
import { join } from 'path'
import { recordArtifact } from './db.js'

/** Format as `2026-04-13_15-42-30-123Z` (filename-safe, sortable). */
function fileStamp(d = new Date()): string {
  return d.toISOString().replace(/[:]/g, '-').replace(/\..+/, `-${d.getMilliseconds()}Z`)
}

/**
 * Snapshot the artifact content into `artifacts/history/stageN/<timestamp>.md`
 * and record the row in the `artifacts` DB table.
 *
 * Returns the snapshot's project-dir-relative path on success, or null on failure.
 */
export async function snapshotArtifact(params: {
  projectId: string
  projectDir: string
  stageId: number
  content: string
  kind?: string
}): Promise<string | null> {
  const { projectId, projectDir, stageId, content, kind = 'stage-done' } = params
  try {
    const relDir = join('artifacts', 'history', `stage${stageId}`)
    const absDir = join(projectDir, relDir)
    await fs.mkdir(absDir, { recursive: true })
    const filename = `${fileStamp()}.md`
    const relPath = join(relDir, filename)
    const absPath = join(projectDir, relPath)
    await fs.writeFile(absPath, content, 'utf8')
    recordArtifact({
      project_id: projectId,
      stage_id: stageId,
      path: relPath,
      kind
    })
    return relPath
  } catch {
    return null
  }
}
