import { readFileSync } from 'node:fs'
import { fileURLToPath } from 'node:url'
import { describe, expect, it } from 'vitest'

const css = readFileSync(fileURLToPath(new URL('../../../src/styles.css', import.meta.url)), 'utf8')

function ruleBody(selector: string): string {
  const start = css.indexOf('\n' + selector + ' {')
  expect(start, `rule not found: ${selector}`).toBeGreaterThan(-1)
  return css.slice(start, css.indexOf('}', start))
}

// 顶栏只是入口条，真正干活的是下面的终端。这里把「一条顶栏 = 5px + 28px + 5px
// + 1px 边框 ≈ 39px」钉住：任何一处悄悄变高，整条就会跟着变高，而这种回退在
// 截图里很难一眼看出来。改前是 52px。
describe('顶栏密度', () => {
  it('keeps the topbar vertical padding tight', () => {
    expect(ruleBody('.topbar')).toContain('padding: 5px var(--mac-sp-4) 5px var(--mac-sp-5)')
  })

  // 三类控件必须同高，否则最高的那个单独把整条撑起来（改前就是窗口按钮 30px
  // 和图标按钮 34px 各自为政）。
  it('sizes every topbar control to the same 28px height', () => {
    const iconBtn = ruleBody('.topbar-btn.topbar-btn-icon')
    expect(iconBtn).toContain('height: 28px')
    expect(iconBtn).toContain('width: 28px')
    expect(iconBtn).toContain('padding-block: 0')

    expect(ruleBody('.workspace-picker')).toContain('min-height: 28px')
    expect(ruleBody('.window-controls-btn')).toContain('height: 28px')
  })

  // 图标按钮里既有文字字形（＋ ⚙ ⏰）也有 SVG（代码审查）。不居中的话 SVG 会被
  // 基线顶偏，视觉上比旁边的字形低一截。
  it('centers icon glyphs and SVGs alike', () => {
    const iconBtn = ruleBody('.topbar-btn.topbar-btn-icon')
    expect(iconBtn).toContain('display: inline-flex')
    expect(iconBtn).toContain('align-items: center')
    expect(iconBtn).toContain('justify-content: center')
  })
})
