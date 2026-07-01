#!/usr/bin/env node
import { spawn } from 'node:child_process'
import { constants } from 'node:fs'
import { access, chmod, copyFile, cp, mkdir, mkdtemp, readFile, readdir, realpath, rm, stat } from 'node:fs/promises'
import { arch, platform, tmpdir } from 'node:os'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { extractZipArchive } from './archive.mjs'

const repoRoot = dirname(dirname(fileURLToPath(import.meta.url)))
const asrRoot = join(repoRoot, 'resources', 'asr')
const modelName = 'ggml-base.bin'
const modelSource = join(repoRoot, 'models', 'whisper', modelName)
const modelDest = join(asrRoot, 'models', modelName)

const whisperWinUrl =
  'https://github.com/ggml-org/whisper.cpp/releases/download/v1.9.1/whisper-bin-x64.zip'
const ffmpegWinUrl =
  'https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-n8.1-latest-win64-lgpl-8.1.zip'
const modelUrl = 'https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-base.bin'

async function exists(path) {
  try {
    await access(path, constants.F_OK)
    return true
  } catch {
    return false
  }
}

function run(command, args, options = {}) {
  return new Promise((resolve, reject) => {
    const child = spawn(command, args, { stdio: 'inherit', ...options })
    child.on('error', reject)
    child.on('close', (code) => {
      if (code === 0) {
        resolve()
        return
      }
      reject(new Error(`${command} ${args.join(' ')} exited ${code}`))
    })
  })
}

function runQuiet(command, args, options = {}) {
  return new Promise((resolve, reject) => {
    const child = spawn(command, args, { stdio: 'ignore', ...options })
    child.on('error', reject)
    child.on('close', (code) => {
      if (code === 0) {
        resolve()
        return
      }
      reject(new Error(`${command} ${args.join(' ')} exited ${code}`))
    })
  })
}

async function firstExisting(paths) {
  for (const path of paths) {
    if (await exists(path)) return path
  }
  return ''
}

function uniqueStrings(values) {
  return Array.from(new Set(values.filter(Boolean)))
}

async function download(url, dest) {
  await mkdir(dirname(dest), { recursive: true })
  await run('curl', ['-L', '--fail', '-o', dest, url])
}

async function unzip(zipPath, destDir) {
  await rm(destDir, { recursive: true, force: true })
  await mkdir(destDir, { recursive: true })
  await extractZipArchive(zipPath, destDir, { run })
}

async function copyExecutable(src, dest) {
  await copyRuntimeFile(src, dest, 0o755)
}

async function copyRuntimeFile(src, dest, mode = 0o755) {
  await mkdir(dirname(dest), { recursive: true })
  await rm(dest, { force: true })
  await copyFile(src, dest)
  await chmod(dest, mode)
}

async function copyModel() {
  if ((await exists(modelDest)) && !(await isGitLfsPointer(modelDest))) return
  await mkdir(dirname(modelDest), { recursive: true })
  if ((await exists(modelSource)) && !(await isGitLfsPointer(modelSource))) {
    await copyFile(modelSource, modelDest)
    return
  }
  console.log(`Downloading ${modelName} for packaged ASR runtime...`)
  await download(modelUrl, modelDest)
}

async function isGitLfsPointer(path) {
  try {
    const info = await stat(path)
    if (info.size > 1024 * 1024) return false
    const content = await readFile(path, 'utf8')
    return content.startsWith('version https://git-lfs.github.com/spec/v1')
  } catch {
    return false
  }
}

async function installNameTool(args) {
  await run('install_name_tool', args)
}

async function tryInstallNameTool(args) {
  try {
    await runQuiet('install_name_tool', args)
  } catch {
    // install_name_tool fails when the old path is not present or an rpath
    // already exists. The script handles several Homebrew layouts, so these
    // best-effort rewrites keep repeated local packaging runs idempotent.
  }
}

async function rewriteDependency(binaryPath, oldPaths, newPath) {
  for (const oldPath of uniqueStrings(oldPaths)) {
    await tryInstallNameTool(['-change', oldPath, newPath, binaryPath])
  }
}

async function codesignAdHoc(path) {
  try {
    await run('codesign', ['--force', '--sign', '-', path])
  } catch {
    // Local development builds can still run unsigned helpers. Packagers may
    // re-sign the full app later, so a best-effort ad-hoc signature is enough.
  }
}

async function prepareDarwinWhisper() {
  if (platform() !== 'darwin') return

  const platformKey = `darwin-${arch()}`
  const binDir = join(asrRoot, platformKey, 'bin')
  const libDir = join(asrRoot, platformKey, 'lib')
  const destCli = join(binDir, 'whisper-cli')

  const cli = await firstExisting(['/opt/homebrew/bin/whisper-cli', '/usr/local/bin/whisper-cli'])
  if (!cli) {
    throw new Error('whisper-cli not found. Install whisper.cpp before packaging on macOS.')
  }
  const realCli = await realpath(cli)
  if (!(await exists(destCli))) {
    await copyExecutable(realCli, destCli)
  }
  await mkdir(libDir, { recursive: true })
  await rm(join(asrRoot, platformKey, 'libexec'), { recursive: true, force: true })

  const whisperPrefix = dirname(dirname(realCli))
  const ggmlPrefix = await firstExisting([
    '/opt/homebrew/opt/ggml',
    '/usr/local/opt/ggml',
    join(whisperPrefix, 'opt', 'ggml')
  ])
  if (!ggmlPrefix) {
    throw new Error('ggml not found. Install ggml before packaging on macOS.')
  }
  const libWhisper = await firstExisting([
    join(whisperPrefix, 'lib', 'libwhisper.1.dylib'),
    '/opt/homebrew/opt/whisper-cpp/lib/libwhisper.1.dylib',
    '/usr/local/opt/whisper-cpp/lib/libwhisper.1.dylib'
  ])
  const libGgml = await firstExisting([
    join(ggmlPrefix, 'lib', 'libggml.0.dylib'),
    '/opt/homebrew/opt/ggml/lib/libggml.0.dylib',
    '/usr/local/opt/ggml/lib/libggml.0.dylib'
  ])
  const libGgmlBase = await firstExisting([
    join(ggmlPrefix, 'lib', 'libggml-base.0.dylib'),
    '/opt/homebrew/opt/ggml/lib/libggml-base.0.dylib',
    '/usr/local/opt/ggml/lib/libggml-base.0.dylib'
  ])
  const libOmp = await firstExisting([
    '/opt/homebrew/opt/libomp/lib/libomp.dylib',
    '/usr/local/opt/libomp/lib/libomp.dylib'
  ])
  for (const [name, src] of Object.entries({
    'libwhisper.1.dylib': libWhisper,
    'libggml.0.dylib': libGgml,
    'libggml-base.0.dylib': libGgmlBase,
    'libomp.dylib': libOmp
  })) {
    if (!src) throw new Error(`Missing macOS ASR library: ${name}`)
    await copyRuntimeFile(src, join(libDir, name), 0o755)
  }

  const destWhisper = join(libDir, 'libwhisper.1.dylib')
  const destGgml = join(libDir, 'libggml.0.dylib')
  const destGgmlBase = join(libDir, 'libggml-base.0.dylib')
  const destOmp = join(libDir, 'libomp.dylib')
  const realLibGgml = await realpath(libGgml)
  const realLibGgmlBase = await realpath(libGgmlBase)
  const realLibOmp = await realpath(libOmp)

  await tryInstallNameTool(['-add_rpath', '@loader_path/../lib', destCli])
  await rewriteDependency(destCli, [libGgml, realLibGgml], '@rpath/libggml.0.dylib')
  await rewriteDependency(destCli, [libGgmlBase, realLibGgmlBase], '@rpath/libggml-base.0.dylib')

  await installNameTool(['-id', '@rpath/libwhisper.1.dylib', destWhisper])
  await rewriteDependency(destWhisper, [libGgml, realLibGgml], '@rpath/libggml.0.dylib')
  await rewriteDependency(destWhisper, [libGgmlBase, realLibGgmlBase], '@rpath/libggml-base.0.dylib')

  await installNameTool(['-id', '@rpath/libggml.0.dylib', destGgml])
  await installNameTool(['-id', '@rpath/libggml-base.0.dylib', destGgmlBase])
  await rewriteDependency(destGgmlBase, [libOmp, realLibOmp], '@rpath/libomp.dylib')
  await installNameTool(['-id', '@rpath/libomp.dylib', destOmp])

  const sourceLibexec = await firstExisting([
    join(ggmlPrefix, 'libexec'),
    '/opt/homebrew/opt/ggml/libexec',
    '/usr/local/opt/ggml/libexec'
  ])
  if (!sourceLibexec) {
    throw new Error('ggml backend plugins not found. Install ggml before packaging on macOS.')
  }
  const backendFiles = (await readdir(sourceLibexec))
    .filter((name) => /^libggml-.*\.so$/.test(name))
  if (backendFiles.length === 0) {
    throw new Error('ggml backend plugins not found. Install ggml before packaging on macOS.')
  }

  const copiedBackends = []
  for (const name of backendFiles) {
    const destBackend = join(binDir, name)
    await copyRuntimeFile(join(sourceLibexec, name), destBackend, 0o755)
    await tryInstallNameTool(['-add_rpath', '@loader_path/../lib', destBackend])
    await rewriteDependency(destBackend, [libGgmlBase, realLibGgmlBase], '@rpath/libggml-base.0.dylib')
    await rewriteDependency(destBackend, [libOmp, realLibOmp], '@rpath/libomp.dylib')
    copiedBackends.push(destBackend)
  }

  for (const path of [destCli, destWhisper, destGgml, destGgmlBase, destOmp, ...copiedBackends]) {
    await codesignAdHoc(path)
  }
}

async function prepareWindowsWhisper() {
  const binDir = join(asrRoot, 'win32-x64', 'bin')
  const destCli = join(binDir, 'whisper-cli.exe')
  if (await exists(destCli)) return

  const tempDir = await mkdtemp(join(tmpdir(), 'multi-ai-asr-whisper-'))
  try {
    const zipPath = join(tempDir, 'whisper-bin-x64.zip')
    await download(whisperWinUrl, zipPath)
    const unzipDir = join(tempDir, 'whisper')
    await unzip(zipPath, unzipDir)
    await mkdir(binDir, { recursive: true })
    await cp(join(unzipDir, 'Release'), binDir, { recursive: true })
  } finally {
    await rm(tempDir, { recursive: true, force: true })
  }
}

async function prepareWindowsFfmpeg() {
  const binDir = join(asrRoot, 'win32-x64', 'bin')
  const destFfmpeg = join(binDir, 'ffmpeg.exe')
  if (await exists(destFfmpeg)) return

  const tempDir = await mkdtemp(join(tmpdir(), 'multi-ai-asr-ffmpeg-'))
  try {
    const zipPath = join(tempDir, 'ffmpeg-win64-lgpl.zip')
    await download(ffmpegWinUrl, zipPath)
    const unzipDir = join(tempDir, 'ffmpeg')
    await unzip(zipPath, unzipDir)
    const srcRoot = join(unzipDir, 'ffmpeg-n8.1-latest-win64-lgpl-8.1')
    await copyExecutable(join(srcRoot, 'bin', 'ffmpeg.exe'), destFfmpeg)
    await mkdir(join(asrRoot, 'win32-x64', 'licenses'), { recursive: true })
    await copyFile(
      join(srcRoot, 'LICENSE.txt'),
      join(asrRoot, 'win32-x64', 'licenses', 'ffmpeg-LICENSE.txt')
    )
  } finally {
    await rm(tempDir, { recursive: true, force: true })
  }
}

async function main() {
  await copyModel()
  await prepareDarwinWhisper()
  await prepareWindowsWhisper()
  await prepareWindowsFfmpeg()
  console.log('ASR runtime assets are ready.')
}

main().catch((error) => {
  console.error(error instanceof Error ? error.message : String(error))
  process.exitCode = 1
})
