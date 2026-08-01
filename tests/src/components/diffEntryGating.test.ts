import { readFileSync } from 'node:fs'
import { fileURLToPath } from 'node:url'
import { describe, expect, it } from 'vitest'

const app = readFileSync(fileURLToPath(new URL('../../../src/App.tsx', import.meta.url)), 'utf8')

/**
 * 「代码审查」入口的开放条件收敛成一句话：当前工作空间是 git 仓库。
 *
 * 它此前被三个无关条件锁着，而且是逐步叠上去的：
 *   - 必须选中普通任务（planName）—— 那是**回灌批注**要写方案文件的前提
 *   - 会话必须运行中          —— 同上，发批注才需要
 *   - mainPanelMounted        —— 会话没起来时按钮整个不渲染，连灰的都看不到
 * 顶栏又不再显示当前普通任务名，于是这些条件全部隐形：按钮要么灰着要么消失，
 * 界面上没有任何解释。这条测试钉住「别再往回加」。
 */
describe('代码审查入口的开放条件', () => {
  function topbarDiffButton(): string {
    const marker = 'aria-label="代码审查"'
    const at = app.indexOf(marker)
    expect(at, '顶栏代码审查按钮不见了').toBeGreaterThan(-1)
    // 往回取整个按钮块（含渲染条件），往后取到按钮结束。
    const start = app.lastIndexOf('{hasProject &&', at)
    expect(start, '按钮的渲染条件不再是 hasProject').toBeGreaterThan(-1)
    return app.slice(start, at + marker.length)
  }

  it('gates the topbar entry on being a git repo, nothing else', () => {
    const block = topbarDiffButton()

    expect(block).toContain('disabled={isGitRepo === false}')
    // 这三个是回灌批注的前提，不该出现在看 diff 的入口条件里。
    expect(block).not.toContain('planName')
    expect(block).not.toContain('sessionStatus')
    expect(block).not.toContain('mainPanelMounted')
  })

  it('detects the git repo from the workspace instead of assuming it', () => {
    // 复用既有的 git:status——非仓库时返回 ok:false，不该为此新开一个 IPC。
    expect(app).toContain('window.api.git.status(targetRepo)')
    expect(app).toContain('setIsGitRepo(res.ok)')
  })

  // 发批注只要求会话在跑（要有人接收），不要求选中任务：普通任务和定时任务
  // 只是「手动发」和「按点发」的区别，没有「当前选中任务」这种状态。
  it('only requires a running session to send annotations', () => {
    const submit = app.slice(
      app.indexOf('const submitDiffAnnotations'),
      app.indexOf('const judgeExternalReviewItem')
    )

    expect(submit).toContain('会话未启动，无法发送批注')
    expect(submit).not.toContain('没有选中普通任务')
    // 有任务就把方案文件当上下文带上，没有就不带——而不是直接拒发。
    expect(submit).toContain("planName.trim() ? getPlanAbsPath(planName.trim()) : undefined")
  })

  it('does not block the external-review judge on a selected task either', () => {
    const judge = app.slice(app.indexOf('const judgeExternalReviewItem'))

    expect(judge).not.toContain("'no plan selected'")
  })
})
