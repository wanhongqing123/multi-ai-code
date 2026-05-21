/**
 * Generates the temporary MCP config file that we pass to Claude Code via
 * `--mcp-config <path>` on session spawn, so the AI gets a `query_kb` tool
 * pointed at this repo's local knowledge base.
 *
 * Codex CLI is unaffected — it doesn't read this file. For Codex the only
 * KB-access path is the digest injection in buildSystemPrompt.
 */

import { promises as fs } from 'fs'
import { join, dirname } from 'path'
import { fileURLToPath } from 'url'
import { tmpdir } from 'os'
import { randomBytes } from 'crypto'
import { dbPath } from '../store/paths.js'

const __dirname = dirname(fileURLToPath(import.meta.url))

/** Returns the absolute path of the bundled MCP server script. */
export function getMcpServerScriptPath(): string {
  // In dev: electron/kb/mcpServer.cjs is shipped beside this module.
  // In packaged builds: the same relative layout — `out/main/kb/mcpServer.cjs`
  // since electron-vite copies non-TS assets through the rollup build.
  return join(__dirname, 'mcpServer.cjs')
}

/**
 * Writes a Claude-Code-style MCP config to a temp file and returns its path.
 * The caller is responsible for passing this path on the CLI via
 * `--mcp-config <path>` and cleaning up when the session ends.
 *
 * Returns null when the bundled mcpServer.cjs script is missing — in that
 * case we silently fall back to digest-only access.
 */
export async function writeKbMcpConfig(repoPath: string): Promise<string | null> {
  const scriptPath = getMcpServerScriptPath()
  try {
    await fs.access(scriptPath)
  } catch {
    return null
  }
  const dir = join(tmpdir(), 'multi-ai-code', 'mcp-configs')
  await fs.mkdir(dir, { recursive: true })
  const file = join(dir, `kb-${randomBytes(6).toString('hex')}.json`)
  const config = {
    mcpServers: {
      'multi-ai-code-kb': {
        command: 'node',
        args: [scriptPath, '--repo', repoPath, '--db', dbPath()]
      }
    }
  }
  await fs.writeFile(file, JSON.stringify(config, null, 2), 'utf8')
  return file
}

/** Best-effort cleanup of a previously-written config file. */
export async function cleanupKbMcpConfig(path: string | null): Promise<void> {
  if (!path) return
  try {
    await fs.unlink(path)
  } catch {
    /* ignore */
  }
}
