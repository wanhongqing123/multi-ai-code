import { promises as fs } from 'fs'
import { join } from 'path'
import { homedir } from 'os'

export function rootDir(): string {
  return process.env.MULTI_AI_ROOT ?? join(homedir(), 'MultiAICode')
}

export function projectsDir(): string {
  return join(rootDir(), 'projects')
}

export function dbPath(): string {
  return join(rootDir(), 'multi-ai-code.db')
}

export function projectDir(projectId: string): string {
  return join(projectsDir(), projectId)
}

export function artifactsDir(projectId: string): string {
  return join(projectDir(projectId), 'artifacts')
}

export function designArchiveDir(targetRepo: string): string {
  return join(targetRepo.replace(/[\/\\]+$/, ''), '.multi-ai-code', 'designs')
}

/**
 * One-shot: for each project that still has a legacy
 * `workspaces/stage1_design/<plan>.md`, copy the file into the new
 * `<target_repo>/.multi-ai-code/designs/<plan>.md` location before the
 * `workspaces/` directory itself is removed in `ensureRootDir`.
 * Safe to call repeatedly; skips when target file already exists.
 */
export async function migrateLegacyStage1Artifacts(): Promise<void> {
  let entries: string[]
  try {
    entries = await fs.readdir(projectsDir())
  } catch {
    return
  }
  for (const pid of entries) {
    const pdir = projectDir(pid)
    const legacyDir = join(pdir, 'workspaces', 'stage1_design')
    let files: string[]
    try {
      files = await fs.readdir(legacyDir)
    } catch {
      continue
    }
    let meta: { target_repo?: string } | null = null
    try {
      meta = JSON.parse(await fs.readFile(join(pdir, 'project.json'), 'utf8'))
    } catch {
      /* missing or unreadable — skip migration for this project */
      continue
    }
    if (!meta?.target_repo) continue
    const dest = designArchiveDir(meta.target_repo)
    try {
      await fs.mkdir(dest, { recursive: true })
    } catch {
      continue
    }
    for (const f of files) {
      if (!f.endsWith('.md')) continue
      // Skip auto-generated system prompt files the legacy backend left in
      // the workspace (e.g., CLAUDE.md written as Claude's system prompt).
      // They're not user plans and would pollute the plan list.
      const base = f.slice(0, -3).toLowerCase()
      if (base === 'claude' || base === 'codex' || base === 'agents') continue
      const src = join(legacyDir, f)
      const tgt = join(dest, f)
      try {
        // skip if target already exists (don't clobber user's current plan)
        await fs.access(tgt)
        continue
      } catch {
        /* target missing — copy */
      }
      try {
        await fs.copyFile(src, tgt)
      } catch {
        /* tolerate per-file failure */
      }
    }
  }
}

export async function ensureRootDir(): Promise<void> {
  await fs.mkdir(projectsDir(), { recursive: true })
  // Salvage any legacy stage-1 design md files before wiping workspaces/.
  await migrateLegacyStage1Artifacts()
  // Retire the platform-managed `workspaces/` subdir from every project.
  // Stage 1 now writes design.md directly to <target_repo>/.multi-ai-code/designs/;
  // Stages 2-4 no longer exist. Safe to remove on every startup.
  const entries = await fs.readdir(projectsDir())
  for (const id of entries) {
    const ws = join(projectsDir(), id, 'workspaces')
    try {
      await fs.rm(ws, { recursive: true, force: true })
    } catch {
      /* force:true already handles ENOENT; this catches EPERM/EBUSY (locked dir) — skip, retry next startup */
    }
  }
}

/**
 * Creates the full directory layout for a new project.
 * Single-stage architecture: creates artifacts dir, design archive dir, and project metadata.
 */
export async function createProjectLayout(
  projectId: string,
  targetRepoPath: string
): Promise<void> {
  const pdir = projectDir(projectId)
  await fs.mkdir(join(pdir, 'artifacts'), { recursive: true })

  // Stage 1 designs archive into target_repo/.multi-ai-code/designs/
  await fs.mkdir(designArchiveDir(targetRepoPath), { recursive: true })

  const historyPath = join(pdir, 'artifacts', 'history.jsonl')
  try {
    await fs.access(historyPath)
  } catch {
    await fs.writeFile(historyPath, '')
  }

  const metaPath = join(pdir, 'project.json')
  try {
    await fs.access(metaPath)
  } catch {
    await fs.writeFile(
      metaPath,
      JSON.stringify(
        {
          id: projectId,
          name: projectId,
          target_repo: targetRepoPath,
          created_at: new Date().toISOString()
        },
        null,
        2
      )
    )
  }
}
