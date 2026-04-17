import { promises as fs } from 'fs'
import { join } from 'path'
import { recordArtifact, recordOrTouchArtifact } from './db.js'

/** Format as `2026-04-13_15-42-30-123Z` (filename-safe, sortable). */
function fileStamp(d = new Date()): string {
  return d.toISOString().replace(/[:]/g, '-').replace(/\..+/, `-${d.getMilliseconds()}Z`)
}

const CURRENT_MARK = '<!-- CURRENT-START -->'
const CURRENT_END = '<!-- CURRENT-END -->'
const HISTORY_MARK = '<!-- HISTORY-START -->'
const HISTORY_END = '<!-- HISTORY-END -->'

/**
 * Merge a new iteration into an existing aggregate file by:
 *   1) promoting the new content to the "当前版本" block at top, and
 *   2) pushing the previous "当前版本" block down into a collapsed
 *      "历史修订" section.
 *
 * The resulting file stays readable at a glance (latest content first)
 * while preserving full iteration history for audit.
 */
function buildAggregateFile(params: {
  label: string
  stageId: number
  newContent: string
  newKind: string
  existing: string | null
}): string {
  const { label, stageId, newContent, newKind, existing } = params
  const now = new Date().toLocaleString()

  const fileHeader = `# ${label} — Stage ${stageId}\n\n> 方案迭代集：顶部永远是**当前最终版本**；历史修订折叠在下方以便审计。\n`

  // Parse existing file (if any) into {previousCurrent, historyBody}
  let previousCurrent = ''
  let previousCurrentMeta = ''
  let historyBody = ''
  if (existing) {
    const curStart = existing.indexOf(CURRENT_MARK)
    const curEnd = existing.indexOf(CURRENT_END)
    if (curStart !== -1 && curEnd > curStart) {
      const block = existing.slice(curStart + CURRENT_MARK.length, curEnd).trim()
      // first line is the meta header "## 当前版本 · time · kind"
      const nl = block.indexOf('\n')
      if (nl >= 0) {
        previousCurrentMeta = block.slice(0, nl).replace(/^##\s*/, '').trim()
        previousCurrent = block.slice(nl + 1).trim()
      } else {
        previousCurrent = block
      }
    }
    const hStart = existing.indexOf(HISTORY_MARK)
    const hEnd = existing.indexOf(HISTORY_END)
    if (hStart !== -1 && hEnd > hStart) {
      historyBody = existing.slice(hStart + HISTORY_MARK.length, hEnd).trim()
    }
  }

  const currentBlock =
    `${CURRENT_MARK}\n## 当前版本 · ${now} · ${newKind}\n\n${newContent.trimEnd()}\n${CURRENT_END}\n`

  let newHistoryBody = historyBody
  if (previousCurrent) {
    const demoted = `### ${previousCurrentMeta || '(未知时间)'}\n\n${previousCurrent}\n`
    newHistoryBody = demoted + (newHistoryBody ? '\n' + newHistoryBody : '')
  }

  const historySection = newHistoryBody
    ? `<details>\n<summary>历史修订（折叠，按新→旧）</summary>\n\n${HISTORY_MARK}\n${newHistoryBody}\n${HISTORY_END}\n</details>\n`
    : ''

  return `${fileHeader}\n${currentBlock}\n${historySection}`
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
  /** Human-readable label used as part of the snapshot filename. */
  label?: string
}): Promise<string | null> {
  const { projectId, projectDir, stageId, content, kind = 'stage-done', label } = params
  try {
    const relDir = join('artifacts', 'history', `stage${stageId}`)
    const absDir = join(projectDir, relDir)
    await fs.mkdir(absDir, { recursive: true })
    const safeName = label
      ? label.replace(/[<>:"/\\|?*\x00-\x1f]/g, '_').replace(/\s+/g, '_').slice(0, 80)
      : ''

    // Any stage with a plan name: aggregate all iterations of the same plan
    // into one file, appending each revision with a timestamped section
    // header. No label → keep the per-timestamp behavior for back-compat.
    const aggregate = !!safeName
    const filename = aggregate
      ? `${safeName}.md`
      : `${fileStamp()}.md`
    const relPath = join(relDir, filename)
    const absPath = join(projectDir, relPath)

    if (aggregate) {
      let existing: string | null = null
      try {
        existing = await fs.readFile(absPath, 'utf8')
      } catch {
        existing = null
      }
      const merged = buildAggregateFile({
        label: label!,
        stageId,
        newContent: content,
        newKind: kind,
        existing
      })
      await fs.writeFile(absPath, merged, 'utf8')
      recordOrTouchArtifact({
        project_id: projectId,
        stage_id: stageId,
        path: relPath,
        kind
      })
    } else {
      await fs.writeFile(absPath, content, 'utf8')
      recordArtifact({
        project_id: projectId,
        stage_id: stageId,
        path: relPath,
        kind
      })
    }
    return relPath
  } catch {
    return null
  }
}
