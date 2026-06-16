import { execFile, spawn } from 'child_process'
import { createHash } from 'crypto'
import { existsSync } from 'fs'
import { mkdir, writeFile } from 'fs/promises'
import { tmpdir } from 'os'
import { basename, join } from 'path'
import { promisify } from 'util'

const execFileAsync = promisify(execFile)

export interface RuntimeDebugOutputCapture {
  stop(): void
}

export interface StartDebugOutputCaptureOptions {
  rootPid: number | null
  env?: NodeJS.ProcessEnv
  onData: (chunk: string) => void
  onDiagnostic?: (message: string) => void
}

type CompilerKind = 'cl' | 'gcc'

interface CompilerCandidate {
  kind: CompilerKind
  command: string
}

const WINDOWS_DEBUG_OUTPUT_CAPTURE_SOURCE = String.raw`
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DBWIN_BUFFER_SIZE 4096
#define DBWIN_DATA_SIZE (DBWIN_BUFFER_SIZE - sizeof(DWORD))

typedef struct {
  DWORD process_id;
  char data[DBWIN_DATA_SIZE];
} DBWIN_BUFFER;

static int pid_matches_root(DWORD pid, DWORD root_pid) {
  if (root_pid == 0 || pid == root_pid) return 1;

  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return 0;

  DWORD current = pid;
  for (int depth = 0; depth < 64; ++depth) {
    PROCESSENTRY32 entry;
    DWORD parent = 0;
    BOOL found = FALSE;
    entry.dwSize = sizeof(entry);

    if (!Process32First(snapshot, &entry)) break;
    do {
      if (entry.th32ProcessID == current) {
        parent = entry.th32ParentProcessID;
        found = TRUE;
        break;
      }
    } while (Process32Next(snapshot, &entry));

    if (!found || parent == 0 || parent == current) break;
    if (parent == root_pid) {
      CloseHandle(snapshot);
      return 1;
    }
    current = parent;
  }

  CloseHandle(snapshot);
  return 0;
}

static char* acp_to_utf8(const char* input) {
  int wide_len = MultiByteToWideChar(CP_ACP, 0, input, -1, NULL, 0);
  if (wide_len <= 0) return NULL;

  WCHAR* wide = (WCHAR*)LocalAlloc(LMEM_FIXED, (SIZE_T)wide_len * sizeof(WCHAR));
  if (!wide) return NULL;
  if (MultiByteToWideChar(CP_ACP, 0, input, -1, wide, wide_len) <= 0) {
    LocalFree(wide);
    return NULL;
  }

  int utf8_len = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
  if (utf8_len <= 0) {
    LocalFree(wide);
    return NULL;
  }

  char* utf8 = (char*)LocalAlloc(LMEM_FIXED, (SIZE_T)utf8_len);
  if (!utf8) {
    LocalFree(wide);
    return NULL;
  }

  if (WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8, utf8_len, NULL, NULL) <= 0) {
    LocalFree(utf8);
    LocalFree(wide);
    return NULL;
  }

  LocalFree(wide);
  return utf8;
}

static size_t bounded_strlen(const char* input, size_t max_len) {
  size_t len = 0;
  while (len < max_len && input[len] != '\0') {
    ++len;
  }
  return len;
}

int main(int argc, char** argv) {
  DWORD root_pid = 0;
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--pid") == 0 && i + 1 < argc) {
      root_pid = (DWORD)strtoul(argv[++i], NULL, 10);
    }
  }

  HANDLE buffer_ready = CreateEventA(NULL, FALSE, FALSE, "DBWIN_BUFFER_READY");
  if (!buffer_ready) {
    fprintf(stderr, "CreateEvent DBWIN_BUFFER_READY failed: %lu\n", GetLastError());
    return 2;
  }

  HANDLE data_ready = CreateEventA(NULL, FALSE, FALSE, "DBWIN_DATA_READY");
  if (!data_ready) {
    fprintf(stderr, "CreateEvent DBWIN_DATA_READY failed: %lu\n", GetLastError());
    CloseHandle(buffer_ready);
    return 2;
  }

  HANDLE mapping = CreateFileMappingA(
    INVALID_HANDLE_VALUE,
    NULL,
    PAGE_READWRITE,
    0,
    DBWIN_BUFFER_SIZE,
    "DBWIN_BUFFER"
  );
  if (!mapping) {
    fprintf(stderr, "CreateFileMapping DBWIN_BUFFER failed: %lu\n", GetLastError());
    CloseHandle(data_ready);
    CloseHandle(buffer_ready);
    return 2;
  }
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    fprintf(stderr, "another DBWIN debug-output listener is already active\n");
    CloseHandle(mapping);
    CloseHandle(data_ready);
    CloseHandle(buffer_ready);
    return 3;
  }

  DBWIN_BUFFER* buffer = (DBWIN_BUFFER*)MapViewOfFile(
    mapping,
    FILE_MAP_READ,
    0,
    0,
    DBWIN_BUFFER_SIZE
  );
  if (!buffer) {
    fprintf(stderr, "MapViewOfFile DBWIN_BUFFER failed: %lu\n", GetLastError());
    CloseHandle(mapping);
    CloseHandle(data_ready);
    CloseHandle(buffer_ready);
    return 2;
  }

  for (;;) {
    SetEvent(buffer_ready);
    DWORD wait_result = WaitForSingleObject(data_ready, 500);
    if (wait_result == WAIT_TIMEOUT) continue;
    if (wait_result == WAIT_FAILED) {
      fprintf(stderr, "WaitForSingleObject DBWIN_DATA_READY failed: %lu\n", GetLastError());
      break;
    }
    if (wait_result != WAIT_OBJECT_0) continue;

    DWORD pid = buffer->process_id;
    if (!pid_matches_root(pid, root_pid)) continue;

    char message[DBWIN_DATA_SIZE + 1];
    memcpy(message, buffer->data, DBWIN_DATA_SIZE);
    message[DBWIN_DATA_SIZE] = '\0';

    size_t len = bounded_strlen(message, DBWIN_DATA_SIZE);
    while (len > 0 && (message[len - 1] == '\r' || message[len - 1] == '\n')) {
      message[--len] = '\0';
    }
    if (len == 0) continue;

    char* utf8 = acp_to_utf8(message);
    printf("[debug:%lu] %s\n", (unsigned long)pid, utf8 ? utf8 : message);
    fflush(stdout);
    if (utf8) LocalFree(utf8);
  }

  UnmapViewOfFile(buffer);
  CloseHandle(mapping);
  CloseHandle(data_ready);
  CloseHandle(buffer_ready);
  return 0;
}
`

const HELPER_HASH = createHash('sha256')
  .update(WINDOWS_DEBUG_OUTPUT_CAPTURE_SOURCE)
  .digest('hex')
  .slice(0, 12)

function mergedEnv(env?: NodeJS.ProcessEnv): NodeJS.ProcessEnv {
  return { ...process.env, ...(env ?? {}) }
}

async function findOnPath(name: string, env: NodeJS.ProcessEnv): Promise<string[]> {
  try {
    const { stdout } = await execFileAsync('where', [name], {
      env,
      windowsHide: true,
      timeout: 5000,
      maxBuffer: 1024 * 1024,
    })
    return String(stdout)
      .split(/\r?\n/)
      .map((line) => line.trim())
      .filter(Boolean)
  } catch {
    return []
  }
}

async function compilerCandidates(env: NodeJS.ProcessEnv): Promise<CompilerCandidate[]> {
  const candidates: CompilerCandidate[] = []
  for (const command of await findOnPath('cl.exe', env)) {
    candidates.push({ kind: 'cl', command })
  }
  for (const command of await findOnPath('gcc.exe', env)) {
    candidates.push({ kind: 'gcc', command })
  }
  for (const command of await findOnPath('clang.exe', env)) {
    candidates.push({ kind: 'gcc', command })
  }

  for (const command of [
    'C:\\msys64\\mingw64\\bin\\gcc.exe',
    'C:\\msys64\\usr\\bin\\gcc.exe',
    'D:\\Program Files\\LLVM\\bin\\clang.exe',
    'C:\\Program Files\\LLVM\\bin\\clang.exe',
  ]) {
    if (existsSync(command) && !candidates.some((candidate) => candidate.command === command)) {
      candidates.push({ kind: 'gcc', command })
    }
  }

  return candidates
}

function compilerArgs(candidate: CompilerCandidate, sourcePath: string, exePath: string): string[] {
  if (candidate.kind === 'cl') {
    return ['/nologo', '/O2', sourcePath, `/Fe:${exePath}`]
  }

  return [sourcePath, '-O2', '-o', exePath]
}

function errorOutput(error: unknown): string {
  const value = error as { stdout?: unknown; stderr?: unknown; message?: unknown }
  const stderr = value.stderr ? String(value.stderr).trim() : ''
  if (stderr) return stderr.slice(0, 500)
  const stdout = value.stdout ? String(value.stdout).trim() : ''
  if (stdout) return stdout.slice(0, 500)
  return String(value.message ?? error).slice(0, 500)
}

async function ensureDebugOutputHelper(env?: NodeJS.ProcessEnv): Promise<string> {
  const cacheDir = join(tmpdir(), 'multi-ai-code', 'runtime-debug-output')
  const sourcePath = join(cacheDir, `debug-output-${HELPER_HASH}.c`)
  const exePath = join(cacheDir, `debug-output-${HELPER_HASH}.exe`)
  if (existsSync(exePath)) return exePath

  await mkdir(cacheDir, { recursive: true })
  await writeFile(sourcePath, WINDOWS_DEBUG_OUTPUT_CAPTURE_SOURCE, 'utf8')

  const nextEnv = mergedEnv(env)
  const errors: string[] = []
  for (const candidate of await compilerCandidates(nextEnv)) {
    try {
      await execFileAsync(candidate.command, compilerArgs(candidate, sourcePath, exePath), {
        cwd: cacheDir,
        env: nextEnv,
        windowsHide: true,
        timeout: 30_000,
        maxBuffer: 1024 * 1024,
      })
      if (existsSync(exePath)) return exePath
      errors.push(`${basename(candidate.command)} did not create helper exe`)
    } catch (error: unknown) {
      errors.push(`${basename(candidate.command)}: ${errorOutput(error)}`)
    }
  }

  throw new Error(
    errors.length > 0
      ? `failed to build Windows debug-output helper: ${errors.join('; ')}`
      : 'no C compiler found for Windows debug-output helper'
  )
}

function stopChild(child: { killed: boolean; kill: () => boolean }): void {
  if (child.killed) return
  try {
    child.kill()
  } catch {
    // Best effort; the runtime runner will continue without debug-output capture.
  }
}

export async function startWindowsDebugOutputCapture(
  options: StartDebugOutputCaptureOptions
): Promise<RuntimeDebugOutputCapture | null> {
  if (process.platform !== 'win32') return null

  let helperPath: string
  try {
    helperPath = await ensureDebugOutputHelper(options.env)
  } catch (error: unknown) {
    options.onDiagnostic?.(
      `[runtime] Windows debug-output capture unavailable: ${
        error instanceof Error ? error.message : String(error)
      }\n`
    )
    return null
  }

  const args =
    typeof options.rootPid === 'number' && Number.isFinite(options.rootPid)
      ? ['--pid', String(options.rootPid)]
      : []
  const child = spawn(helperPath, args, {
    env: mergedEnv(options.env),
    windowsHide: true,
    stdio: ['ignore', 'pipe', 'pipe'],
  })
  let stopped = false

  child.stdout.on('data', (chunk: Buffer | string) => {
    options.onData(typeof chunk === 'string' ? chunk : chunk.toString('utf8'))
  })
  child.stderr.on('data', (chunk: Buffer | string) => {
    const message = typeof chunk === 'string' ? chunk : chunk.toString('utf8')
    if (message.trim()) options.onDiagnostic?.(`[runtime] ${message}`)
  })
  child.on('error', (error) => {
    if (!stopped) options.onDiagnostic?.(`[runtime] debug-output capture error: ${error.message}\n`)
  })
  child.on('close', (code, signal) => {
    if (!stopped && code !== 0) {
      options.onDiagnostic?.(
        `[runtime] debug-output capture exited (${code ?? signal ?? 'unknown'})\n`
      )
    }
  })

  return {
    stop(): void {
      stopped = true
      stopChild(child)
    },
  }
}
