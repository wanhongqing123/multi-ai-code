import { app } from 'electron'
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

const STAGE_DIR_NAME: Record<number, string> = {
  1: 'stage1_design',
  2: 'stage2_impl',
  3: 'stage3_acceptance',
  4: 'stage4_test'
}

export function workspaceDir(projectId: string, stageId: number): string {
  return join(projectDir(projectId), 'workspaces', STAGE_DIR_NAME[stageId])
}

export function artifactsDir(projectId: string): string {
  return join(projectDir(projectId), 'artifacts')
}

export function designArchiveDir(targetRepo: string): string {
  return join(targetRepo.replace(/[\/\\]+$/, ''), '.multi-ai-code', 'designs')
}

export async function ensureRootDir(): Promise<void> {
  await fs.mkdir(projectsDir(), { recursive: true })
}

/**
 * Creates the full directory layout for a new project.
 * Stages 1/2: independent empty dirs.
 * Stages 3-6: symlinks to target_repo.
 */
export async function createProjectLayout(
  projectId: string,
  targetRepoPath: string
): Promise<void> {
  const pdir = projectDir(projectId)
  await fs.mkdir(join(pdir, 'workspaces'), { recursive: true })
  await fs.mkdir(join(pdir, 'artifacts'), { recursive: true })

  // Stage 1 designs now archive into target_repo/.multi-ai-code/designs/
  // (previously lived in workspaces/stage1_design). The workspace dir below
  // is still created because Stage 1 uses it as an isolated empty cwd.
  await fs.mkdir(designArchiveDir(targetRepoPath), { recursive: true })

  // Stage 1 (design) runs in an isolated empty dir
  await fs.mkdir(workspaceDir(projectId, 1), { recursive: true })

  // Stages 2-4 are symlinks to target_repo
  for (const stageId of [2, 3, 4]) {
    const link = workspaceDir(projectId, stageId)
    try {
      await fs.symlink(targetRepoPath, link, 'dir')
    } catch (err: any) {
      if (err.code !== 'EEXIST') throw err
    }
  }

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
