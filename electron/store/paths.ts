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

export async function ensureRootDir(): Promise<void> {
  await fs.mkdir(projectsDir(), { recursive: true })
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
