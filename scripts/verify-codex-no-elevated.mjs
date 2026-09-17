#!/usr/bin/env node
// 验证 codex 的 elevated 沙箱路径确实关着，且没误伤其它档位。
//
// 观测路径选择（踩过两次才找对）：
//   codex --version  不读 config.toml —— 连拼错的取值都不报错，全部误判为通过
//   codex doctor     读配置但对解析失败容错，而且有警告时本来就返回 1，
//                    拿返回码当证据不成立
//   codex exec       真正加载完整配置，解析失败会直接以错误退出 ← 用这个
//
// 每次改动沙箱相关配置后跑一遍：node scripts/verify-codex-no-elevated.mjs
import { spawnSync } from 'node:child_process'
import { mkdtempSync, writeFileSync, existsSync, statSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { join } from 'node:path'

const codex = process.argv[2] ??
  join(
    process.cwd(),
    'third_party/aicli/codex/codex-rs/target/debug',
    process.platform === 'win32' ? 'codex.exe' : 'codex'
  )

if (!existsSync(codex)) {
  console.error(`找不到 codex：${codex}`)
  console.error('先构建：node scripts/build-aicli-codex.mjs')
  process.exit(1)
}

const failures = []
const check = (name, ok, extra = '') => {
  console.log(`  [${ok ? 'OK' : 'FAIL'}] ${name}${extra ? `  ${extra}` : ''}`)
  if (!ok) failures.push(name)
}

function runWithConfig(tomlBody) {
  const home = mkdtempSync(join(tmpdir(), 'codexhome-'))
  writeFileSync(join(home, 'config.toml'), tomlBody, 'utf8')
  // exec 会加载完整配置；给一个空 prompt 让它尽早退出，我们只关心配置阶段。
  const result = spawnSync(codex, ['exec', ''], {
    env: { ...process.env, CODEX_HOME: home },
    encoding: 'utf8',
    timeout: 120_000
  })
  return { code: result.status, out: `${result.stdout ?? ''}${result.stderr ?? ''}` }
}

const CONFIG_ERROR = 'Error loading config.toml'

console.log('== 0. 自检：这条观测路径能不能看见配置错误 ==')
{
  // 故意写一个不存在的取值。这都看不见的话，后面全是假的。
  const { out } = runWithConfig('[windows]\nsandbox = "definitely-not-a-level"\n')
  const sees = out.includes(CONFIG_ERROR)
  check('exec 会因配置解析失败而报错', sees, sees ? '' : out.trim().slice(0, 200))
  if (!sees) {
    console.log('\n观测路径无效，后面的检查没有意义。')
    process.exit(1)
  }
}

console.log('== 1. elevated 必须被拒绝 ==')
{
  const { code, out } = runWithConfig('[windows]\nsandbox = "elevated"\n')
  check('非零退出码', code !== 0, `rc=${code}`)
  check('报的是配置加载错误', out.includes(CONFIG_ERROR))
  check('说清了不可用', out.includes('不可用'))
  check('给出了替代方案', out.includes('unelevated'))
  check('说明了原因', out.includes('23.7MB'))
  check('指出了出错的键', out.includes('windows.sandbox'))
}

console.log('== 2. unelevated 不能被误伤 ==')
{
  const { out } = runWithConfig('[windows]\nsandbox = "unelevated"\n')
  check('没有配置加载错误', !out.includes(CONFIG_ERROR), out.trim().slice(0, 160))
}

console.log('== 3. 不写 sandbox 键（产品默认路径）==')
{
  const { out } = runWithConfig('')
  check('没有配置加载错误', !out.includes(CONFIG_ERROR), out.trim().slice(0, 160))
}

console.log('== 4. 两个 elevated 专属可执行文件确实不再被构建 ==')
{
  // 不能只看"文件在不在"：target/ 里会留着改动之前的旧产物，
  // 那会让这一项恒假。判据是**比 codex.exe 旧**——
  // codex.exe 每次构建都会重新产出，比它旧就说明这一轮没有构建它们。
  const dir = join(codex, '..')
  const codexMtime = statSync(codex).mtimeMs
  for (const name of ['codex-windows-sandbox-setup', 'codex-command-runner']) {
    const exe = join(dir, process.platform === 'win32' ? `${name}.exe` : name)
    if (!existsSync(exe)) {
      check(`${name} 未产出`, true)
      continue
    }
    const age = statSync(exe).mtimeMs
    check(
      `${name} 是改动前的旧产物（本轮未构建）`,
      age < codexMtime,
      `它 ${new Date(age).toLocaleString()} / codex ${new Date(codexMtime).toLocaleString()}`
    )
  }
}

console.log()
if (failures.length > 0) {
  console.log(`失败 ${failures.length} 项：${failures.join('、')}`)
  process.exit(1)
}
console.log('elevated 路径已关闭，其余档位未受影响')
