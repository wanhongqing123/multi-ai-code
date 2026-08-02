import { promises as fs } from 'fs'
import { join } from 'path'
import { homedir } from 'os'
import { createHash } from 'crypto'
import { joinWithRootStyle } from '../pathStyle.js'

const BASE_DIR_NAME = 'multi-ai-code'

/**
 * 全局基目录（账号无关）。所有账号的数据子目录都挂在它下面；账号注册表等
 * 登录前需要的东西也放这一层。MULTI_AI_ROOT 覆盖时同样作为“基”，账号仍嵌在其下。
 */
export function baseDir(): string {
  return process.env.MULTI_AI_ROOT ?? join(homedir(), BASE_DIR_NAME)
}

let activeAccountId: string | null = null

/** 把用户输入的 desktopUserId 归一为文件系统安全的账号目录名。 */
export function sanitizeAccountId(userId: string): string {
  const trimmed = (userId ?? '').trim()
  if (/^[A-Za-z0-9_-]{1,64}$/.test(trimmed)) return trimmed
  return `acct-${createHash('sha1').update(trimmed).digest('hex').slice(0, 16)}`
}

/** 绑定当前进程的账号（登录成功后由 bind-account 调用）；传 null 解绑。 */
export function setActiveAccount(userId: string | null): void {
  activeAccountId = userId ? sanitizeAccountId(userId) : null
}

export function getActiveAccount(): string | null {
  return activeAccountId
}

/**
 * 当前账号的数据根 = baseDir()/accounts/<账号>。DB、projects、imcli 桥接等全部派生
 * 自此。未绑定账号即调用视为越过登录门的逻辑错误，直接抛出以便及早暴露。
 */
export function rootDir(): string {
  if (!activeAccountId) {
    throw new Error('rootDir() called before an account was bound (login required first)')
  }
  return join(baseDir(), 'accounts', activeAccountId)
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

export async function ensureRootDir(): Promise<void> {
  await fs.mkdir(projectsDir(), { recursive: true })
  // Retire the platform-managed `workspaces/` subdir from every project.
  // 分阶段流程与方案文档都已删除，工作台不再往用户仓库写任何东西；
  // 这里只负责清掉老版本残留的 workspaces/。每次启动跑一遍是安全的。
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
