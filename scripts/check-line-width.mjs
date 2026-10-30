// 检查仓库自有源码的 120 字符行宽；引入的第三方源码与其生成数据保持上游原样。
import { execFileSync } from 'node:child_process'
import { existsSync, readFileSync } from 'node:fs'

const extensions = new Set(['c', 'cpp', 'h', 'hpp', 'java', 'js', 'jsx', 'mjs', 'mm', 'py', 'swift', 'ts', 'tsx'])
const upstreamParts = [
  '/third_party/', '/vendor/', '/Frameworks/', '/build/', '/generated/', '/node_modules/', '/dist/'
]

const tracked = execFileSync('git', ['ls-files', '-z'], {
  encoding: 'utf8', maxBuffer: 16 * 1024 * 1024
}).split('\0').filter(Boolean)
let violations = 0
for (const path of tracked) {
  const extension = path.split('.').pop()
  if (!extensions.has(extension) || upstreamParts.some(part => path.includes(part)) ||
      !existsSync(path)) continue
  const lines = readFileSync(path, 'utf8').split(/\r?\n/)
  lines.forEach((line, index) => {
    const width = Array.from(line).length
    if (width <= 120) return
    process.stderr.write(`${path}:${index + 1}: ${width} characters (maximum 120)\n`)
    violations += 1
  })
}
if (violations) process.exitCode = 1
else process.stdout.write('First-party source lines are within 120 characters.\n')
