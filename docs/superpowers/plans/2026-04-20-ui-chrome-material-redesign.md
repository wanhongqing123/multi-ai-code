# UI Chrome/Material Redesign — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把 `src/styles.css` 从现有暗色+渐变基调整体迁移到 Chrome/Material 风格（浅灰底 + 白卡片 + Google Blue），保留暗色主题作为可切换选项；覆盖主窗口 + 全部 17 个组件；不改 React 组件结构。

**Architecture:** 在 `styles.css` 顶部注入 `--mac-*` CSS 变量层（`:root` 浅色默认 + `:root.theme-dark` 覆盖），所有组件规则改为引用变量。组件迁移按族分批：topbar → tile/grid → shared modal shell → drawer → toast/errorPanel/inputs → diff/review/onboarding → 最后加 React 层的 theme toggle + localStorage 持久化。

**Tech Stack:** CSS 变量、React 18、TypeScript、Electron 33、Vitest。

**Spec reference:** `docs/superpowers/specs/2026-04-20-ui-chrome-material-redesign-design.md`

---

## File Structure

**Modify:**
- `src/styles.css` — 全部 CSS 重写，按 7 个 task 分批（单文件）
- `src/App.tsx` — 加一个顶栏 theme toggle 按钮 + 启动时应用持久化主题（Task 7）
- `src/main.tsx` — 启动前把 `theme-dark` class 打到 `<html>`（防止首帧闪白）(Task 7)
- `src/components/CommandPalette.tsx` — 注册 `Toggle theme` 命令条目（Task 7）

**Create:**
- `src/utils/theme.ts` — `getTheme()`/`setTheme()`/`toggleTheme()` 工具函数（Task 7）
- `src/utils/theme.test.ts` — 单测（Task 7）

**不新建任何组件文件，不改任何组件 props / JSX 结构（除 Task 7 显式列出的两处）。**

---

## Conventions

- **TDD 不适用于纯 CSS 改动**。每个 CSS task 的节奏是：① 读老规则 ② 贴新规则 ③ `npm run dev` 手动验证 ④ commit。
- 只有 Task 7 的 `src/utils/theme.ts` 有单测（走完整 TDD）。
- 每个 task 结束后跑 `npm run typecheck` 和 `npm run test`，必须全绿。
- 每个 task 完成后 commit 一次，commit 消息按 task 标题。
- **定位 CSS 块**：用 Grep 工具 `^\.<class>\b` 找到起始行，连同整个花括号块替换。不要用字符串匹配整块老 CSS —— 它会因为换行或空格差异失败。

---

## Task 1: 引入 `--mac-*` tokens 与 dark theme 层

**Files:**
- Modify: `src/styles.css:1-5`（文件顶部）

**Goal:** 只加变量层，所有组件规则保持不变。本 task 结束时视觉**零变化**（因为没任何规则用 `--mac-*`）。

- [ ] **Step 1: 在 `src/styles.css` 第 1 行之前插入变量块**

在 `* { box-sizing: border-box; }` 之前插入下面全部内容：

```css
/* ==========================================================================
   Design tokens — Chrome/Material redesign
   See: docs/superpowers/specs/2026-04-20-ui-chrome-material-redesign-design.md
   ========================================================================== */

:root {
  /* Surface */
  --mac-bg:            #F1F3F4;
  --mac-bg-subtle:     #E8EAED;
  --mac-surface:       #FFFFFF;
  --mac-surface-raised:#FAFBFC;

  /* Foreground */
  --mac-fg:            #202124;
  --mac-fg-muted:      #3C4043;
  --mac-fg-subtle:     #5F6368;
  --mac-fg-disabled:   #80868B;

  /* Primary (Google Blue) */
  --mac-primary:       #1A73E8;
  --mac-primary-hover: #1967D2;
  --mac-primary-active:#174EA6;
  --mac-primary-soft:  #E8F0FE;
  --mac-primary-on:    #FFFFFF;

  /* Status */
  --mac-success:       #1E8E3E;
  --mac-success-soft:  #E6F4EA;
  --mac-danger:        #D93025;
  --mac-danger-soft:   #FCE8E6;
  --mac-warning:       #B06000;
  --mac-warning-soft:  #FEF7E0;
  --mac-info:          #1967D2;
  --mac-info-soft:     #E8F0FE;

  /* Border */
  --mac-border:        #DADCE0;
  --mac-border-subtle: #E8EAED;
  --mac-border-strong: #BDC1C6;

  /* Focus */
  --mac-focus-ring:    rgba(26, 115, 232, 0.35);

  /* Diff */
  --mac-diff-add-bg:   #E6F4EA;
  --mac-diff-add-fg:   #1E8E3E;
  --mac-diff-del-bg:   #FCE8E6;
  --mac-diff-del-fg:   #D93025;

  /* Typography */
  --mac-font-sans: -apple-system, BlinkMacSystemFont, 'Segoe UI',
                   'PingFang SC', 'Microsoft YaHei', 'Noto Sans CJK SC', sans-serif;
  --mac-font-mono: 'JetBrains Mono', 'Roboto Mono', Menlo, Consolas, monospace;

  --mac-text-xl:   20px;
  --mac-text-lg:   16px;
  --mac-text-md:   14px;
  --mac-text-sm:   13px;
  --mac-text-xs:   12px;
  --mac-text-mono: 11px;

  --mac-weight-regular: 400;
  --mac-weight-medium:  500;
  --mac-weight-bold:    700;

  --mac-line-tight:   1.3;
  --mac-line-normal:  1.5;
  --mac-line-relaxed: 1.7;

  /* Shape */
  --mac-r-sm:   4px;
  --mac-r-md:   8px;
  --mac-r-lg:  10px;
  --mac-r-xl:  14px;
  --mac-r-pill:999px;

  /* Spacing */
  --mac-sp-1: 4px;
  --mac-sp-2: 8px;
  --mac-sp-3: 12px;
  --mac-sp-4: 16px;
  --mac-sp-5: 20px;
  --mac-sp-6: 24px;
  --mac-sp-7: 32px;

  /* Elevation */
  --mac-elev-1: 0 1px 2px rgba(60, 64, 67, 0.05);
  --mac-elev-2: 0 2px 6px rgba(60, 64, 67, 0.10);
  --mac-elev-3: 0 8px 24px rgba(60, 64, 67, 0.15);

  /* Motion */
  --mac-dur-fast:   120ms;
  --mac-dur-normal: 180ms;
  --mac-ease:       cubic-bezier(0.2, 0, 0, 1);
}

:root.theme-dark {
  --mac-bg:            #202124;
  --mac-bg-subtle:     #292A2D;
  --mac-surface:       #292A2D;
  --mac-surface-raised:#35363A;

  --mac-fg:            #E8EAED;
  --mac-fg-muted:      #BDC1C6;
  --mac-fg-subtle:     #9AA0A6;
  --mac-fg-disabled:   #5F6368;

  --mac-primary:       #8AB4F8;
  --mac-primary-hover: #A8C7FA;
  --mac-primary-active:#669DF6;
  --mac-primary-soft:  rgba(138, 180, 248, 0.16);
  --mac-primary-on:    #202124;

  --mac-success:       #81C995;
  --mac-success-soft:  rgba(129, 201, 149, 0.16);
  --mac-danger:        #F28B82;
  --mac-danger-soft:   rgba(242, 139, 130, 0.16);
  --mac-warning:       #FDD663;
  --mac-warning-soft:  rgba(253, 214, 99, 0.16);
  --mac-info:          #8AB4F8;
  --mac-info-soft:     rgba(138, 180, 248, 0.16);

  --mac-border:        #3C4043;
  --mac-border-subtle: #2A2D30;
  --mac-border-strong: #5F6368;

  --mac-focus-ring:    rgba(138, 180, 248, 0.45);

  --mac-diff-add-bg:   rgba(129, 201, 149, 0.12);
  --mac-diff-add-fg:   #81C995;
  --mac-diff-del-bg:   rgba(242, 139, 130, 0.12);
  --mac-diff-del-fg:   #F28B82;

  --mac-elev-1: 0 1px 2px rgba(0, 0, 0, 0.30);
  --mac-elev-2: 0 2px 6px rgba(0, 0, 0, 0.40);
  --mac-elev-3: 0 8px 24px rgba(0, 0, 0, 0.50);
}
```

- [ ] **Step 2: 验证没破坏现有视觉**

```bash
npm run dev
```

目视确认：应用启动，主窗口外观**和改动前完全一致**（暗色、渐变、原配色全在——因为没有规则用到新变量）。用浏览器/Electron DevTools 的 Elements 面板检查 `<html>` 上已自动存在 `--mac-bg: #F1F3F4` 等变量值。

- [ ] **Step 3: 跑类型检查和单测**

```bash
npm run typecheck
npm run test
```

Expected: 两个都全绿。

- [ ] **Step 4: Commit**

```bash
git add src/styles.css
git commit -m "feat(styles): introduce --mac-* tokens & dark theme layer"
```

---

## Task 2: 迁移 topbar + topbar 按钮

**Files:**
- Modify: `src/styles.css`（`.topbar`, `.topbar::after`, `.topbar h1`, `.topbar .meta`, `.meta code`, `.meta strong`, `.meta-warn`, `.topbar-btn`, `.topbar-btn:hover`, `.topbar-btn:active`, `.topbar-btn.topbar-btn-danger`, `.topbar-btn.topbar-btn-primary`, `.topbar-btn:disabled`）

- [ ] **Step 1: 定位现有 topbar 块**

Grep `^\.topbar\b` 找到每个规则块起始行。现有范围约在 `src/styles.css:34-185`。逐个替换为下面的新规则。

- [ ] **Step 2: 替换 topbar 容器**

```css
.topbar {
  display: flex;
  align-items: center;
  gap: var(--mac-sp-3);
  padding: 10px var(--mac-sp-5);
  background: var(--mac-surface);
  border-bottom: 1px solid var(--mac-border);
  color: var(--mac-fg);
}

/* 删掉 .topbar::after 整块（不要那条蓝色光条） */
```

删除 `.topbar::after { ... }` 规则整段。

- [ ] **Step 3: 替换 topbar 标题**

```css
.topbar h1 {
  font-size: var(--mac-text-md);
  margin: 0;
  font-weight: var(--mac-weight-medium);
  letter-spacing: normal;
  color: var(--mac-fg);
  background: none;
  -webkit-background-clip: unset;
  background-clip: unset;
  -webkit-text-fill-color: currentColor;
  display: flex;
  align-items: center;
  gap: var(--mac-sp-2);
}

.topbar h1::before {
  content: 'M';
  display: inline-flex;
  align-items: center;
  justify-content: center;
  width: 24px;
  height: 24px;
  border-radius: var(--mac-r-md);
  background: var(--mac-primary);
  color: var(--mac-primary-on);
  font-size: 13px;
  font-weight: var(--mac-weight-bold);
}
```

- [ ] **Step 4: 替换 topbar meta 与 meta-warn**

```css
.topbar .meta {
  flex: 1;
  font-size: var(--mac-text-xs);
  color: var(--mac-fg-subtle);
  display: inline-flex;
  align-items: center;
  gap: var(--mac-sp-2);
  padding: 6px var(--mac-sp-3);
  background: var(--mac-bg);
  border-radius: var(--mac-r-md);
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
  max-width: max-content;
}

.meta code {
  background: transparent;
  color: var(--mac-fg-subtle);
  padding: 0;
  border: none;
  font-family: var(--mac-font-mono);
  font-size: var(--mac-text-mono);
}

.meta strong {
  color: var(--mac-fg);
  font-weight: var(--mac-weight-medium);
  font-size: var(--mac-text-sm);
}

.meta-warn {
  color: var(--mac-warning);
  background: var(--mac-warning-soft);
  border: 1px solid transparent;
  padding: 2px var(--mac-sp-2);
  border-radius: var(--mac-r-sm);
  font-size: var(--mac-text-xs);
  font-weight: var(--mac-weight-medium);
}
```

- [ ] **Step 5: 替换 topbar 按钮**

```css
.topbar-btn {
  background: var(--mac-surface);
  color: var(--mac-fg-muted);
  border: 1px solid var(--mac-border);
  padding: 6px var(--mac-sp-3);
  border-radius: var(--mac-r-md);
  cursor: pointer;
  font-size: var(--mac-text-xs);
  font-weight: var(--mac-weight-medium);
  letter-spacing: normal;
  transition: background var(--mac-dur-fast) var(--mac-ease),
              border-color var(--mac-dur-fast) var(--mac-ease),
              color var(--mac-dur-fast) var(--mac-ease);
  box-shadow: none;
}

.topbar-btn:hover:not(:disabled) {
  background: var(--mac-bg);
  border-color: var(--mac-border-strong);
  box-shadow: none;
  transform: none;
}

.topbar-btn:active:not(:disabled) {
  background: var(--mac-bg-subtle);
  transform: none;
  box-shadow: none;
}

.topbar-btn:focus-visible {
  outline: none;
  box-shadow: 0 0 0 3px var(--mac-focus-ring);
}

.topbar-btn.topbar-btn-primary {
  background: var(--mac-primary);
  color: var(--mac-primary-on);
  border-color: transparent;
}

.topbar-btn.topbar-btn-primary:hover:not(:disabled) {
  background: var(--mac-primary-hover);
  border-color: transparent;
}

.topbar-btn.topbar-btn-primary:active:not(:disabled) {
  background: var(--mac-primary-active);
}

.topbar-btn.topbar-btn-danger {
  background: var(--mac-danger);
  color: #fff;
  border-color: transparent;
}

.topbar-btn.topbar-btn-danger:hover:not(:disabled) {
  background: var(--mac-danger);
  filter: brightness(1.08);
  border-color: transparent;
}

.topbar-btn:disabled {
  opacity: 1;
  cursor: not-allowed;
  background: var(--mac-bg-subtle);
  color: var(--mac-fg-disabled);
  border-color: var(--mac-border-subtle);
  box-shadow: none;
}
```

- [ ] **Step 6: 更新 `html, body, #root` 全局底色**

Grep `html, body, #root` 找到规则，把 `background: #181a24;` 改为 `background: var(--mac-bg);`，`color: #e6e6e6;` 改为 `color: var(--mac-fg);`，`font-family` 改为 `var(--mac-font-sans)`。

- [ ] **Step 7: 启动 dev server，目视验证**

```bash
npm run dev
```

检查项：
- 顶栏底色浅白、下沿一条极淡灰线（无渐变、无光条）
- 标题左侧有 24px 蓝色 "M" 方块
- meta 信息变成灰色胶囊
- 运行按钮实心蓝色，中止按钮红色，其他按钮 outlined
- hover 不再"弹起"

- [ ] **Step 8: 跑类型检查和单测**

```bash
npm run typecheck
npm run test
```

- [ ] **Step 9: Commit**

```bash
git add src/styles.css
git commit -m "refactor(styles): migrate topbar + topbar buttons to --mac-* tokens"
```

---

## Task 3: 迁移 4 宫格 + stage 面板

**Files:**
- Modify: `src/styles.css`（`.grid`, `.grid.grid-zoomed`, `.tile`, `.tile-head`, `.tile-id`, `.tile-name`, `.tile-badge`, `.tile-body`, `.tile-btn`, `.tile-progress`, `.tile-error`, `.tile.tile-hidden`, `.tile.tile-zoomed`, `.stage-*` 若有）

- [ ] **Step 1: 替换 `.grid` 容器**

```css
.grid {
  display: grid;
  grid-template-columns: repeat(2, 1fr);
  grid-template-rows: repeat(2, 1fr);
  gap: var(--mac-sp-3);
  padding: var(--mac-sp-3);
  background: var(--mac-bg);
  overflow: hidden;
  min-width: 0;
  min-height: 0;
}
```

`.grid.grid-zoomed`, `.tile.tile-hidden`, `.tile.tile-zoomed` 保持结构不动（只控制布局），不要改它们。

- [ ] **Step 2: 替换 `.tile` 卡片**

```css
.tile {
  background: var(--mac-surface);
  border: 1px solid var(--mac-border);
  border-radius: var(--mac-r-lg);
  display: flex;
  flex-direction: column;
  overflow: hidden;
  box-shadow: var(--mac-elev-1);
  transition: box-shadow var(--mac-dur-fast) var(--mac-ease);
}

.tile:hover {
  box-shadow: var(--mac-elev-2);
}
```

- [ ] **Step 3: 替换 `.tile-head`**

```css
.tile-head {
  display: flex;
  align-items: center;
  gap: var(--mac-sp-2);
  padding: 10px var(--mac-sp-3);
  background: var(--mac-surface-raised);
  border-bottom: 1px solid var(--mac-border-subtle);
  font-size: var(--mac-text-sm);
  color: var(--mac-fg);
}
```

- [ ] **Step 4: 替换 `.tile-id` / `.tile-name` / `.tile-badge`**

```css
.tile-id {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  width: 24px;
  height: 24px;
  border-radius: var(--mac-r-pill);
  background: var(--mac-bg-subtle);
  color: var(--mac-fg-subtle);
  font-size: var(--mac-text-xs);
  font-weight: var(--mac-weight-bold);
  flex-shrink: 0;
}

.tile.tile-active .tile-id,
.tile[data-status="running"] .tile-id {
  background: var(--mac-primary-soft);
  color: var(--mac-primary);
}

.tile-name {
  font-size: var(--mac-text-md);
  font-weight: var(--mac-weight-medium);
  color: var(--mac-fg);
}

.tile-badge {
  display: inline-flex;
  align-items: center;
  gap: var(--mac-sp-1);
  padding: 2px var(--mac-sp-2);
  border-radius: var(--mac-r-pill);
  font-size: var(--mac-text-xs);
  font-weight: var(--mac-weight-medium);
  background: var(--mac-bg-subtle);
  color: var(--mac-fg-subtle);
}

.tile-badge.tile-badge-running {
  background: var(--mac-success-soft);
  color: var(--mac-success);
}
.tile-badge.tile-badge-running::before {
  content: '';
  width: 6px;
  height: 6px;
  border-radius: var(--mac-r-pill);
  background: var(--mac-success);
  display: inline-block;
}
.tile-badge.tile-badge-failed {
  background: var(--mac-danger-soft);
  color: var(--mac-danger);
}
.tile-badge.tile-badge-done {
  background: var(--mac-info-soft);
  color: var(--mac-info);
}
```

- [ ] **Step 5: 替换 `.tile-body` / `.tile-btn` / `.tile-progress` / `.tile-error`**

```css
.tile-body {
  flex: 1;
  overflow: hidden;
  display: flex;
  flex-direction: column;
  background: var(--mac-surface);
  color: var(--mac-fg-muted);
  min-height: 0;
}

.tile-btn {
  background: transparent;
  border: 1px solid var(--mac-border);
  color: var(--mac-fg-muted);
  padding: 4px var(--mac-sp-2);
  border-radius: var(--mac-r-md);
  cursor: pointer;
  font-size: var(--mac-text-xs);
  transition: background var(--mac-dur-fast) var(--mac-ease);
}

.tile-btn:hover:not(:disabled) {
  background: var(--mac-bg);
}

.tile-btn:focus-visible {
  outline: none;
  box-shadow: 0 0 0 3px var(--mac-focus-ring);
}

.tile-btn:disabled {
  color: var(--mac-fg-disabled);
  cursor: not-allowed;
}

.tile-progress {
  height: 3px;
  background: var(--mac-bg-subtle);
  overflow: hidden;
}

.tile-progress-fill {
  height: 100%;
  background: var(--mac-primary);
  transition: width var(--mac-dur-normal) var(--mac-ease);
}

.tile-error {
  padding: var(--mac-sp-2) var(--mac-sp-3);
  background: var(--mac-danger-soft);
  color: var(--mac-danger);
  font-size: var(--mac-text-xs);
  border-bottom: 1px solid var(--mac-border-subtle);
}
```

如果 styles.css 里还有 `.tile-progress-fill` 之外的相关规则（如 `.tile-progress-bar`），grep 它们并统一用变量重写；类名不改。

- [ ] **Step 6: 替换 `.plan-progress-*`（若存在）**

Grep `^\.plan-progress` 找到现有规则，整组替换为：

```css
.plan-name-bar {
  display: flex;
  align-items: center;
  gap: var(--mac-sp-3);
  padding: var(--mac-sp-2) var(--mac-sp-4);
  background: var(--mac-surface-raised);
  border-bottom: 1px solid var(--mac-border-subtle);
}

.plan-name-input {
  flex: 1;
  background: var(--mac-surface);
  border: 1px solid var(--mac-border);
  border-radius: var(--mac-r-md);
  padding: 6px var(--mac-sp-3);
  color: var(--mac-fg);
  font-size: var(--mac-text-sm);
}

.plan-name-input:focus {
  outline: none;
  border-color: var(--mac-primary);
  box-shadow: 0 0 0 3px var(--mac-focus-ring);
}

.plan-progress-bar {
  display: flex;
  align-items: center;
  gap: var(--mac-sp-2);
  font-size: var(--mac-text-xs);
  color: var(--mac-fg-subtle);
}

.plan-progress-node {
  width: 10px;
  height: 10px;
  border-radius: var(--mac-r-pill);
  background: var(--mac-bg-subtle);
  border: 1px solid var(--mac-border);
}

.plan-progress-node.plan-progress-done {
  background: var(--mac-success);
  border-color: var(--mac-success);
}

.plan-progress-node.plan-progress-current {
  background: var(--mac-primary);
  border-color: var(--mac-primary);
  box-shadow: 0 0 0 3px var(--mac-primary-soft);
}

.plan-progress-sep {
  flex: 1;
  height: 1px;
  background: var(--mac-border);
  min-width: 12px;
  max-width: 40px;
}

.plan-progress-label {
  font-weight: var(--mac-weight-medium);
  color: var(--mac-fg-muted);
}
```

- [ ] **Step 7: 验证**

```bash
npm run dev
```

目视：4 个 stage 卡片白底、圆角、细边框、非常轻阴影；阶段圆编号徽章；运行中卡片头有绿色小圆点 chip。

- [ ] **Step 8: 类型检查 + 单测**

```bash
npm run typecheck
npm run test
```

- [ ] **Step 9: Commit**

```bash
git add src/styles.css
git commit -m "refactor(styles): migrate 4-tile grid + stage panel to tokens"
```

---

## Task 4: 统一 modal / dialog 共用壳 + drawer

**Files:**
- Modify: `src/styles.css`（`.modal-backdrop`, `.modal`, `.modal-head`, `.modal-head h3`, `.modal-close`, `.modal-body`, `.modal-field`, `.modal-field select`, `.modal-field textarea`, `.modal-checkbox`, `.modal-error`, `.modal-actions`, `.modal-actions .drawer-btn`, `.drawer`, `.drawer-head`, `.drawer-body`, `.drawer-title`, `.drawer-close`, `.drawer-btn`, `.drawer-actions`, `.drawer-meta`, `.drawer-stage`, `.drawer-empty`, `.cmdk-modal`, `.cmdk-input`, `.cmdk-list`, `.cmdk-item`, `.cmdk-hint`, `.cmdk-label`）

- [ ] **Step 1: 替换 modal-backdrop**

```css
.modal-backdrop {
  position: fixed;
  inset: 0;
  background: rgba(32, 33, 36, 0.5);
  display: flex;
  align-items: center;
  justify-content: center;
  z-index: 1000;
  animation: mac-fade-in var(--mac-dur-fast) var(--mac-ease);
}

:root.theme-dark .modal-backdrop {
  background: rgba(0, 0, 0, 0.6);
}

@keyframes mac-fade-in {
  from { opacity: 0; }
  to { opacity: 1; }
}
```

- [ ] **Step 2: 替换 `.modal` 容器 + `.modal-head` / `.modal-body` / `.modal-actions`**

```css
.modal {
  width: 520px;
  max-width: 90vw;
  max-height: 85vh;
  background: var(--mac-surface);
  border: none;
  border-radius: var(--mac-r-xl);
  box-shadow: var(--mac-elev-3);
  display: flex;
  flex-direction: column;
  overflow: hidden;
  color: var(--mac-fg);
}

.modal-head {
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: var(--mac-sp-4) var(--mac-sp-5);
  background: var(--mac-surface);
  border-bottom: 1px solid var(--mac-border-subtle);
}

.modal-head h3 {
  margin: 0;
  font-size: var(--mac-text-lg);
  font-weight: var(--mac-weight-medium);
  color: var(--mac-fg);
}

.modal-close {
  background: transparent;
  border: none;
  color: var(--mac-fg-subtle);
  font-size: 20px;
  cursor: pointer;
  padding: 4px var(--mac-sp-2);
  line-height: 1;
  border-radius: var(--mac-r-md);
  transition: background var(--mac-dur-fast) var(--mac-ease);
}

.modal-close:hover {
  background: var(--mac-bg);
  color: var(--mac-fg);
}

.modal-close:focus-visible {
  outline: none;
  box-shadow: 0 0 0 3px var(--mac-focus-ring);
}

.modal-body {
  padding: var(--mac-sp-5);
  overflow: auto;
  display: flex;
  flex-direction: column;
  gap: var(--mac-sp-3);
  color: var(--mac-fg-muted);
  font-size: var(--mac-text-sm);
}

.modal-field {
  display: flex;
  flex-direction: column;
  gap: var(--mac-sp-1);
  font-size: var(--mac-text-xs);
  color: var(--mac-fg-subtle);
}

.modal-field select,
.modal-field textarea,
.modal-field input[type='text'] {
  background: var(--mac-surface);
  border: 1px solid var(--mac-border);
  border-radius: var(--mac-r-md);
  padding: 8px var(--mac-sp-3);
  color: var(--mac-fg);
  font-size: var(--mac-text-sm);
  font-family: var(--mac-font-sans);
  resize: vertical;
}

.modal-field textarea {
  font-family: var(--mac-font-mono);
  min-height: 120px;
}

.modal-field select:focus,
.modal-field textarea:focus,
.modal-field input[type='text']:focus {
  outline: none;
  border-color: var(--mac-primary);
  box-shadow: 0 0 0 3px var(--mac-focus-ring);
}

.modal-checkbox {
  display: flex;
  align-items: center;
  gap: var(--mac-sp-2);
  font-size: var(--mac-text-sm);
  color: var(--mac-fg-muted);
  cursor: pointer;
  accent-color: var(--mac-primary);
}

.modal-error {
  padding: var(--mac-sp-2) var(--mac-sp-5);
  background: var(--mac-danger-soft);
  color: var(--mac-danger);
  font-size: var(--mac-text-xs);
  border-bottom: 1px solid var(--mac-border-subtle);
}

.modal-actions {
  display: flex;
  gap: var(--mac-sp-2);
  padding: var(--mac-sp-3) var(--mac-sp-5);
  background: var(--mac-surface);
  border-top: 1px solid var(--mac-border-subtle);
  justify-content: flex-end;
}

.modal-actions .drawer-btn {
  flex: 0 0 auto;
  min-width: 88px;
}
```

- [ ] **Step 3: 替换 `.drawer` 基础 + 内部元素**

```css
.drawer {
  position: fixed;
  top: 0;
  right: 0;
  bottom: 0;
  width: 420px;
  max-width: 90vw;
  background: var(--mac-surface);
  border-left: 1px solid var(--mac-border);
  box-shadow: var(--mac-elev-3);
  display: flex;
  flex-direction: column;
  overflow: hidden;
  z-index: 1000;
  color: var(--mac-fg);
  animation: mac-slide-in var(--mac-dur-normal) var(--mac-ease);
}

@keyframes mac-slide-in {
  from { transform: translateX(100%); }
  to { transform: translateX(0); }
}

.drawer-head {
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: var(--mac-sp-4) var(--mac-sp-5);
  background: var(--mac-surface);
  border-bottom: 1px solid var(--mac-border-subtle);
}

.drawer-title {
  margin: 0;
  font-size: var(--mac-text-lg);
  font-weight: var(--mac-weight-medium);
  color: var(--mac-fg);
}

.drawer-close {
  background: transparent;
  border: none;
  color: var(--mac-fg-subtle);
  font-size: 20px;
  cursor: pointer;
  padding: 4px var(--mac-sp-2);
  border-radius: var(--mac-r-md);
}

.drawer-close:hover {
  background: var(--mac-bg);
  color: var(--mac-fg);
}

.drawer-body {
  flex: 1;
  overflow: auto;
  padding: var(--mac-sp-5);
  color: var(--mac-fg-muted);
  font-size: var(--mac-text-sm);
  display: flex;
  flex-direction: column;
  gap: var(--mac-sp-3);
}

.drawer-btn {
  background: var(--mac-surface);
  color: var(--mac-fg-muted);
  border: 1px solid var(--mac-border);
  padding: 7px var(--mac-sp-4);
  border-radius: var(--mac-r-md);
  cursor: pointer;
  font-size: var(--mac-text-sm);
  font-weight: var(--mac-weight-medium);
  transition: background var(--mac-dur-fast) var(--mac-ease);
}

.drawer-btn:hover:not(:disabled) {
  background: var(--mac-bg);
  border-color: var(--mac-border-strong);
}

.drawer-btn:focus-visible {
  outline: none;
  box-shadow: 0 0 0 3px var(--mac-focus-ring);
}

.drawer-btn.drawer-btn-primary {
  background: var(--mac-primary);
  color: var(--mac-primary-on);
  border-color: transparent;
}

.drawer-btn.drawer-btn-primary:hover:not(:disabled) {
  background: var(--mac-primary-hover);
}

.drawer-btn:disabled {
  background: var(--mac-bg-subtle);
  color: var(--mac-fg-disabled);
  cursor: not-allowed;
  border-color: var(--mac-border-subtle);
}

.drawer-actions {
  display: flex;
  gap: var(--mac-sp-2);
  padding: var(--mac-sp-3) var(--mac-sp-5);
  border-top: 1px solid var(--mac-border-subtle);
  justify-content: flex-end;
}

.drawer-meta,
.drawer-stage,
.drawer-stage-name,
.drawer-verdict,
.drawer-empty {
  color: var(--mac-fg-subtle);
  font-size: var(--mac-text-xs);
}

.drawer-done-badge {
  display: inline-flex;
  align-items: center;
  padding: 2px var(--mac-sp-2);
  border-radius: var(--mac-r-pill);
  background: var(--mac-success-soft);
  color: var(--mac-success);
  font-size: var(--mac-text-xs);
  font-weight: var(--mac-weight-medium);
}
```

如果 `.drawer-artifact`, `.drawer-error`, `.drawer-git*` 这些类存在，按相同 tokens 风格重写（每个规则只替换颜色/边框/圆角即可）。

- [ ] **Step 4: 替换 `.cmdk-*` command palette**

```css
.cmdk-modal {
  width: 640px;
  max-width: 92vw;
  max-height: 70vh;
  background: var(--mac-surface);
  border: none;
  border-radius: var(--mac-r-xl);
  box-shadow: var(--mac-elev-3);
  display: flex;
  flex-direction: column;
  overflow: hidden;
  color: var(--mac-fg);
}

.cmdk-input {
  width: 100%;
  border: none;
  padding: var(--mac-sp-4) var(--mac-sp-5);
  background: transparent;
  color: var(--mac-fg);
  font-size: var(--mac-text-md);
  font-family: var(--mac-font-sans);
  outline: none;
  border-bottom: 1px solid var(--mac-border-subtle);
}

.cmdk-list {
  flex: 1;
  overflow: auto;
  padding: var(--mac-sp-1) 0;
}

.cmdk-label {
  padding: var(--mac-sp-2) var(--mac-sp-5) var(--mac-sp-1);
  font-size: var(--mac-text-xs);
  color: var(--mac-fg-subtle);
  font-weight: var(--mac-weight-medium);
  text-transform: uppercase;
  letter-spacing: 0.5px;
}

.cmdk-item {
  display: flex;
  align-items: center;
  gap: var(--mac-sp-3);
  padding: var(--mac-sp-2) var(--mac-sp-5);
  cursor: pointer;
  color: var(--mac-fg);
  font-size: var(--mac-text-sm);
}

.cmdk-item:hover,
.cmdk-item[aria-selected='true'] {
  background: var(--mac-primary-soft);
  color: var(--mac-primary);
}

.cmdk-hint {
  font-family: var(--mac-font-mono);
  font-size: var(--mac-text-mono);
  color: var(--mac-fg-subtle);
  padding: 1px var(--mac-sp-2);
  border: 1px solid var(--mac-border);
  border-radius: var(--mac-r-sm);
  background: var(--mac-bg);
}
```

- [ ] **Step 5: 验证**

```bash
npm run dev
```

打开每个 modal 各目视一次：Settings、Templates、Doctor、Feedback、FilePreview、PlanReview、DiffViewer、ProjectPicker、StageSettings。打开 Timeline、Completion drawer。按 ⌘K 打开 CommandPalette。

- [ ] **Step 6: 类型检查 + 单测**

```bash
npm run typecheck
npm run test
```

- [ ] **Step 7: Commit**

```bash
git add src/styles.css
git commit -m "refactor(styles): unify modal shell + drawer + command palette"
```

---

## Task 5: 迁移 toast / errorPanel / 通用输入

**Files:**
- Modify: `src/styles.css`（`.toast-host`, `.toast`, `.toast-msg`, `.toast-close`, `.toast-action`, `.toast-success`, `.toast-warn`, `.toast-error`, `.error-panel`, `.error-panel-head`, `.error-panel-body`, `.error-level`, `.error-msg`, `.error-boundary*`, `.severity-*`, 剩余 `input`/`select`/`textarea` 规则）

- [ ] **Step 1: 替换 Toast 家族**

```css
.toast-host {
  position: fixed;
  top: var(--mac-sp-5);
  right: var(--mac-sp-5);
  display: flex;
  flex-direction: column;
  gap: var(--mac-sp-2);
  z-index: 1100;
  pointer-events: none;
}

.toast {
  pointer-events: auto;
  min-width: 280px;
  max-width: 420px;
  padding: var(--mac-sp-3) var(--mac-sp-4);
  background: var(--mac-surface-raised);
  border: 1px solid var(--mac-border);
  border-radius: var(--mac-r-md);
  box-shadow: var(--mac-elev-2);
  display: flex;
  align-items: flex-start;
  gap: var(--mac-sp-3);
  color: var(--mac-fg);
  font-size: var(--mac-text-sm);
  position: relative;
  overflow: hidden;
}

.toast::before {
  content: '';
  position: absolute;
  left: 0;
  top: 0;
  bottom: 0;
  width: 4px;
  background: var(--mac-info);
}

.toast.toast-success::before { background: var(--mac-success); }
.toast.toast-warn::before    { background: var(--mac-warning); }
.toast.toast-error::before   { background: var(--mac-danger); }

.toast-msg {
  flex: 1;
  color: var(--mac-fg-muted);
}

.toast-action {
  background: transparent;
  border: none;
  color: var(--mac-primary);
  cursor: pointer;
  font-size: var(--mac-text-sm);
  font-weight: var(--mac-weight-medium);
  padding: 0;
}

.toast-action:hover {
  color: var(--mac-primary-hover);
}

.toast-close {
  background: transparent;
  border: none;
  color: var(--mac-fg-subtle);
  cursor: pointer;
  font-size: 16px;
  line-height: 1;
  padding: 0 var(--mac-sp-1);
}

.toast-close:hover {
  color: var(--mac-fg);
}
```

- [ ] **Step 2: 替换 ErrorPanel**

```css
.error-panel {
  position: fixed;
  right: var(--mac-sp-5);
  bottom: var(--mac-sp-5);
  width: 520px;
  max-height: 60vh;
  background: var(--mac-surface);
  border: 1px solid var(--mac-border);
  border-radius: var(--mac-r-lg);
  box-shadow: var(--mac-elev-3);
  display: flex;
  flex-direction: column;
  overflow: hidden;
  z-index: 1050;
  color: var(--mac-fg);
}

.error-panel-head {
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: var(--mac-sp-3) var(--mac-sp-4);
  background: var(--mac-surface-raised);
  border-bottom: 1px solid var(--mac-border-subtle);
  font-size: var(--mac-text-sm);
  font-weight: var(--mac-weight-medium);
}

.error-panel-body {
  flex: 1;
  overflow: auto;
  padding: var(--mac-sp-2) 0;
  font-family: var(--mac-font-mono);
  font-size: var(--mac-text-mono);
  color: var(--mac-fg-muted);
}

.error-level {
  display: inline-flex;
  align-items: center;
  gap: var(--mac-sp-1);
  padding: 1px var(--mac-sp-2);
  border-radius: var(--mac-r-sm);
  font-family: var(--mac-font-sans);
  font-size: var(--mac-text-xs);
  font-weight: var(--mac-weight-medium);
  text-transform: uppercase;
}

.error-level.error-level-warn {
  background: var(--mac-warning-soft);
  color: var(--mac-warning);
}

.error-level.error-level-error {
  background: var(--mac-danger-soft);
  color: var(--mac-danger);
}

.error-level.error-level-info {
  background: var(--mac-info-soft);
  color: var(--mac-info);
}

.error-msg {
  padding: 2px var(--mac-sp-4);
  color: var(--mac-fg-muted);
  white-space: pre-wrap;
  word-break: break-word;
}
```

若 `.severity-*` 仍被其他组件引用，同样用 `--mac-<status>-soft` + `--mac-<status>` 映射。

- [ ] **Step 3: 替换 ErrorBoundary**

```css
.error-boundary {
  min-height: 100vh;
  display: flex;
  align-items: center;
  justify-content: center;
  background: var(--mac-bg);
  padding: var(--mac-sp-5);
}

.error-boundary-card {
  background: var(--mac-surface);
  border: 1px solid var(--mac-border);
  border-radius: var(--mac-r-xl);
  box-shadow: var(--mac-elev-2);
  padding: var(--mac-sp-6);
  max-width: 640px;
  width: 100%;
  color: var(--mac-fg);
}

.error-boundary-msg {
  color: var(--mac-danger);
  font-weight: var(--mac-weight-medium);
  margin: var(--mac-sp-2) 0;
  font-size: var(--mac-text-md);
}

.error-boundary-stack {
  background: var(--mac-bg);
  border: 1px solid var(--mac-border-subtle);
  border-radius: var(--mac-r-md);
  padding: var(--mac-sp-3);
  font-family: var(--mac-font-mono);
  font-size: var(--mac-text-mono);
  color: var(--mac-fg-subtle);
  max-height: 240px;
  overflow: auto;
  white-space: pre-wrap;
}

.error-boundary-hint {
  color: var(--mac-fg-subtle);
  font-size: var(--mac-text-xs);
  margin-top: var(--mac-sp-3);
}

.error-boundary-actions {
  display: flex;
  gap: var(--mac-sp-2);
  margin-top: var(--mac-sp-4);
  justify-content: flex-end;
}
```

- [ ] **Step 4: 扫尾未变量化的 input / select / textarea**

Grep `^input\b`, `^select\b`, `^textarea\b`, `input\[type` 等。若有独立规则没被 `.modal-field` / 其他规则覆盖，套用相同 pattern：

```css
input[type='text'],
input[type='search'],
input[type='number'],
select,
textarea {
  background: var(--mac-surface);
  border: 1px solid var(--mac-border);
  border-radius: var(--mac-r-md);
  padding: 6px var(--mac-sp-3);
  color: var(--mac-fg);
  font-size: var(--mac-text-sm);
  font-family: var(--mac-font-sans);
}

input:focus,
select:focus,
textarea:focus {
  outline: none;
  border-color: var(--mac-primary);
  box-shadow: 0 0 0 3px var(--mac-focus-ring);
}
```

- [ ] **Step 5: 验证**

```bash
npm run dev
```

- 触发 4 种 toast（可在 DevTools 里 `window.__showToast = ...` 或正常跑 stage 完成）
- 打开 ErrorPanel（点顶栏错误按钮）
- 模拟一个 React 错误（改一处 throw 再恢复）看 ErrorBoundary
- 随便找个带 input 的 dialog 看 focus ring

- [ ] **Step 6: 类型检查 + 单测**

```bash
npm run typecheck
npm run test
```

- [ ] **Step 7: Commit**

```bash
git add src/styles.css
git commit -m "refactor(styles): migrate toast, error panel, inputs to tokens"
```

---

## Task 6: 迁移 diff / review / onboarding / 杂项 dialogs

**Files:**
- Modify: `src/styles.css`（`.diff-*`, `.dv-*`, `.diff-viewer-modal`, `.review-*`, `.onboarding-*`, `.onb-*`, `.plan-review-*`, `.file-preview-*`, `.gs-*`, `.global-search-modal`, `.history-*`, `.timeline-*`, `.templates-*`, `.doctor-*`, `.project-*`, `.stage-settings-*`, `.md-rendered`, `.term-*`, `.msys-*`, `.drop-hint`）

本 task 是扫尾，对**剩下没碰过的类全部按相同 pattern 走一遍**。

- [ ] **Step 1: DiffView + DiffViewerDialog**

```css
.diff-view,
.diff-viewer-modal {
  background: var(--mac-surface);
  color: var(--mac-fg);
  font-family: var(--mac-font-mono);
  font-size: var(--mac-text-mono);
}

.diff-viewer-modal {
  width: 92vw;
  max-width: 1280px;
  height: 86vh;
  border-radius: var(--mac-r-xl);
  border: none;
  box-shadow: var(--mac-elev-3);
}

.diff-head,
.diff-head-old,
.diff-head-new {
  background: var(--mac-surface-raised);
  color: var(--mac-fg-muted);
  padding: var(--mac-sp-1) var(--mac-sp-3);
  border-bottom: 1px solid var(--mac-border-subtle);
  font-weight: var(--mac-weight-medium);
}

.diff-body {
  background: var(--mac-surface);
  overflow: auto;
}

.diff-line {
  display: flex;
  padding: 0 var(--mac-sp-3);
  line-height: 1.5;
}

.diff-add {
  background: var(--mac-diff-add-bg);
  color: var(--mac-diff-add-fg);
}

.diff-del {
  background: var(--mac-diff-del-bg);
  color: var(--mac-diff-del-fg);
}

.diff-same {
  color: var(--mac-fg-muted);
}
```

把 `.dv-*`（DiffViewerDialog 内部布局）的颜色/边框一次性走变量：grep `^\.dv-` 找到所有规则，把背景色 → `--mac-surface` / `--mac-surface-raised`，边框 → `--mac-border` / `--mac-border-subtle`，文字 → `--mac-fg` / `--mac-fg-muted` / `--mac-fg-subtle`，按钮 → 用 `.drawer-btn` 的规格（同样边框 + padding + 圆角）。示例：

```css
.dv-toolbar {
  display: flex;
  align-items: center;
  gap: var(--mac-sp-2);
  padding: var(--mac-sp-2) var(--mac-sp-4);
  background: var(--mac-surface-raised);
  border-bottom: 1px solid var(--mac-border-subtle);
}

.dv-mode-tabs {
  display: inline-flex;
  border: 1px solid var(--mac-border);
  border-radius: var(--mac-r-md);
  overflow: hidden;
}

.dv-mode-tab {
  padding: 4px var(--mac-sp-3);
  background: var(--mac-surface);
  color: var(--mac-fg-muted);
  font-size: var(--mac-text-xs);
  cursor: pointer;
  border: none;
  border-right: 1px solid var(--mac-border-subtle);
}

.dv-mode-tab:last-child { border-right: none; }

.dv-mode-tab[aria-selected='true'],
.dv-mode-tab.dv-mode-tab-active {
  background: var(--mac-primary-soft);
  color: var(--mac-primary);
}

.dv-nav-btn,
.dv-refresh-btn {
  background: var(--mac-surface);
  border: 1px solid var(--mac-border);
  color: var(--mac-fg-muted);
  padding: 4px var(--mac-sp-3);
  border-radius: var(--mac-r-md);
  cursor: pointer;
  font-size: var(--mac-text-xs);
}

.dv-nav-btn:hover:not(:disabled),
.dv-refresh-btn:hover:not(:disabled) {
  background: var(--mac-bg);
}

.dv-file-head {
  background: var(--mac-surface-raised);
  color: var(--mac-fg);
  padding: var(--mac-sp-2) var(--mac-sp-3);
  border-bottom: 1px solid var(--mac-border-subtle);
  font-weight: var(--mac-weight-medium);
  font-size: var(--mac-text-sm);
}

.dv-row {
  display: flex;
  font-family: var(--mac-font-mono);
  font-size: var(--mac-text-mono);
  line-height: 1.5;
}

.dv-cell,
.dv-cell-left,
.dv-cell-right {
  padding: 0 var(--mac-sp-3);
  white-space: pre;
  overflow: hidden;
}

.dv-placeholder,
.dv-error {
  padding: var(--mac-sp-5);
  color: var(--mac-fg-subtle);
  text-align: center;
  font-size: var(--mac-text-sm);
}
```

- [ ] **Step 2: ReviewChecklist**

```css
.review-checklist {
  background: var(--mac-surface);
  color: var(--mac-fg);
}

.review-toolbar {
  display: flex;
  align-items: center;
  gap: var(--mac-sp-2);
  padding: var(--mac-sp-2) var(--mac-sp-4);
  background: var(--mac-surface-raised);
  border-bottom: 1px solid var(--mac-border-subtle);
  font-size: var(--mac-text-xs);
  color: var(--mac-fg-subtle);
}

.review-toolbar-actions {
  margin-left: auto;
  display: flex;
  gap: var(--mac-sp-2);
}

.review-list {
  padding: var(--mac-sp-2) 0;
}

.review-item {
  padding: var(--mac-sp-2) var(--mac-sp-4);
  border-bottom: 1px solid var(--mac-border-subtle);
  cursor: pointer;
}

.review-item:hover {
  background: var(--mac-bg);
}

.review-item-head {
  display: flex;
  align-items: flex-start;
  gap: var(--mac-sp-2);
  font-size: var(--mac-text-sm);
  color: var(--mac-fg);
}

.review-item-toggle {
  accent-color: var(--mac-primary);
  margin-top: 2px;
}

.review-item-title {
  flex: 1;
  font-weight: var(--mac-weight-medium);
}

.review-item[data-checked='true'] .review-item-title,
.review-item.review-item-checked .review-item-title {
  color: var(--mac-fg-subtle);
  text-decoration: line-through;
}

.review-item-loc {
  font-family: var(--mac-font-mono);
  font-size: var(--mac-text-mono);
  color: var(--mac-fg-subtle);
}

.review-item-body {
  padding: var(--mac-sp-1) 0 0 var(--mac-sp-6);
  font-size: var(--mac-text-xs);
  color: var(--mac-fg-muted);
}

.review-empty {
  padding: var(--mac-sp-6);
  text-align: center;
  color: var(--mac-fg-subtle);
  font-size: var(--mac-text-sm);
}
```

- [ ] **Step 3: OnboardingWizard + `.onb-*`**

```css
.onboarding-modal {
  width: 720px;
  max-width: 92vw;
}

.onboarding-steps {
  display: flex;
  flex-direction: column;
  gap: var(--mac-sp-2);
  padding: var(--mac-sp-4);
  border-right: 1px solid var(--mac-border-subtle);
  min-width: 200px;
}

.onb-step {
  display: flex;
  align-items: center;
  gap: var(--mac-sp-2);
  padding: var(--mac-sp-2) var(--mac-sp-3);
  border-radius: var(--mac-r-md);
  font-size: var(--mac-text-sm);
  color: var(--mac-fg-muted);
  cursor: pointer;
}

.onb-step.onb-step-active {
  background: var(--mac-primary-soft);
  color: var(--mac-primary);
  font-weight: var(--mac-weight-medium);
}

.onb-step.onb-step-done {
  color: var(--mac-fg-subtle);
}

.onb-num {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  width: 20px;
  height: 20px;
  border-radius: var(--mac-r-pill);
  background: var(--mac-bg-subtle);
  color: var(--mac-fg-subtle);
  font-size: var(--mac-text-xs);
  font-weight: var(--mac-weight-bold);
}

.onb-step.onb-step-active .onb-num {
  background: var(--mac-primary);
  color: var(--mac-primary-on);
}

.onb-step.onb-step-done .onb-num {
  background: var(--mac-success);
  color: #fff;
}

.onb-sep {
  height: 1px;
  background: var(--mac-border-subtle);
  margin: var(--mac-sp-1) 0;
}

.onb-hint {
  color: var(--mac-fg-subtle);
  font-size: var(--mac-text-xs);
}

.onboarding-body {
  flex: 1;
  padding: var(--mac-sp-5);
  overflow: auto;
}
```

- [ ] **Step 4: PlanReviewDialog `.plan-review-*`**

把所有 `.plan-review-*` 规则的颜色/边框/圆角走变量，按 pattern：
- 背景 `--mac-surface`/`--mac-surface-raised`
- 边框 `--mac-border`/`--mac-border-subtle`
- 文字 `--mac-fg`/`--mac-fg-muted`/`--mac-fg-subtle`
- 按钮沿用 `.drawer-btn` 规格

示例关键规则：

```css
.plan-review-modal {
  width: 96vw;
  max-width: 1400px;
  height: 88vh;
}

.plan-review-body,
.plan-review-pane {
  background: var(--mac-surface);
}

.plan-review-side {
  background: var(--mac-surface-raised);
  border-left: 1px solid var(--mac-border-subtle);
  min-width: 320px;
}

.plan-review-side-title {
  padding: var(--mac-sp-3) var(--mac-sp-4);
  border-bottom: 1px solid var(--mac-border-subtle);
  font-size: var(--mac-text-md);
  font-weight: var(--mac-weight-medium);
  color: var(--mac-fg);
}

.plan-review-item {
  padding: var(--mac-sp-3) var(--mac-sp-4);
  border-bottom: 1px solid var(--mac-border-subtle);
  cursor: pointer;
}

.plan-review-item:hover {
  background: var(--mac-bg);
}

.plan-review-item-head {
  display: flex;
  align-items: center;
  gap: var(--mac-sp-2);
  font-size: var(--mac-text-sm);
  color: var(--mac-fg);
}

.plan-review-item-idx {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  width: 22px;
  height: 22px;
  border-radius: var(--mac-r-pill);
  background: var(--mac-primary-soft);
  color: var(--mac-primary);
  font-size: var(--mac-text-xs);
  font-weight: var(--mac-weight-bold);
}

.plan-review-item-quote {
  font-family: var(--mac-font-mono);
  font-size: var(--mac-text-mono);
  color: var(--mac-fg-subtle);
  background: var(--mac-bg);
  padding: var(--mac-sp-2);
  border-radius: var(--mac-r-md);
  margin-top: var(--mac-sp-2);
}

.plan-review-item-comment {
  padding: var(--mac-sp-2) 0;
  font-size: var(--mac-text-sm);
  color: var(--mac-fg-muted);
}

.plan-review-item-btn {
  background: transparent;
  border: none;
  color: var(--mac-fg-subtle);
  cursor: pointer;
  padding: 4px var(--mac-sp-2);
  border-radius: var(--mac-r-sm);
}

.plan-review-item-btn:hover {
  background: var(--mac-bg);
  color: var(--mac-fg);
}

.plan-review-empty {
  padding: var(--mac-sp-6);
  text-align: center;
  color: var(--mac-fg-subtle);
}

.plan-review-general {
  padding: var(--mac-sp-4);
  border-top: 1px solid var(--mac-border-subtle);
}

.plan-review-general-label {
  font-size: var(--mac-text-xs);
  color: var(--mac-fg-subtle);
  margin-bottom: var(--mac-sp-1);
}

.plan-review-general-input {
  width: 100%;
  background: var(--mac-surface);
  border: 1px solid var(--mac-border);
  border-radius: var(--mac-r-md);
  padding: var(--mac-sp-2);
  color: var(--mac-fg);
  font-size: var(--mac-text-sm);
  font-family: var(--mac-font-mono);
  min-height: 80px;
  resize: vertical;
}

.plan-review-composer-backdrop {
  position: fixed;
  inset: 0;
  background: rgba(32, 33, 36, 0.4);
  z-index: 1200;
  display: flex;
  align-items: center;
  justify-content: center;
}

.plan-review-composer {
  background: var(--mac-surface);
  border: 1px solid var(--mac-border);
  border-radius: var(--mac-r-xl);
  box-shadow: var(--mac-elev-3);
  width: 520px;
  max-width: 92vw;
}

.plan-review-composer-head {
  padding: var(--mac-sp-3) var(--mac-sp-4);
  border-bottom: 1px solid var(--mac-border-subtle);
  font-size: var(--mac-text-md);
  font-weight: var(--mac-weight-medium);
}

.plan-review-composer-quote {
  padding: var(--mac-sp-3) var(--mac-sp-4);
  font-family: var(--mac-font-mono);
  font-size: var(--mac-text-mono);
  color: var(--mac-fg-subtle);
  background: var(--mac-bg);
  margin: 0 var(--mac-sp-4);
  border-radius: var(--mac-r-md);
}

.plan-review-composer-input {
  width: calc(100% - 2 * var(--mac-sp-4));
  margin: var(--mac-sp-3) var(--mac-sp-4);
  background: var(--mac-surface);
  border: 1px solid var(--mac-border);
  border-radius: var(--mac-r-md);
  padding: var(--mac-sp-2);
  font-size: var(--mac-text-sm);
  font-family: var(--mac-font-sans);
  min-height: 80px;
  resize: vertical;
}

.plan-review-composer-actions {
  display: flex;
  justify-content: flex-end;
  gap: var(--mac-sp-2);
  padding: var(--mac-sp-3) var(--mac-sp-4);
  border-top: 1px solid var(--mac-border-subtle);
}

.plan-review-annotate-floater {
  position: absolute;
  padding: 4px var(--mac-sp-2);
  background: var(--mac-primary);
  color: var(--mac-primary-on);
  border-radius: var(--mac-r-md);
  box-shadow: var(--mac-elev-2);
  font-size: var(--mac-text-xs);
  cursor: pointer;
  z-index: 1100;
}
```

- [ ] **Step 5: 剩余 dialogs — 批量扫尾**

对下列类做相同的"颜色/边框/背景/padding 全部走变量"的机械替换；类名不动；pattern 同上：

- `.file-preview-*` — FilePreviewDialog
- `.gs-*` / `.global-search-modal` — GlobalSearchDialog
- `.history-*` — History / TimelineDrawer 列表
- `.timeline-*` — TimelineDrawer 专用
- `.templates-*` — TemplatesDialog
- `.doctor-*` — DoctorDialog
- `.project-*` — ProjectPicker
- `.stage-settings-*` — StageSettingsDialog
- `.md-rendered` — react-markdown 渲染容器（正文色 `--mac-fg-muted`，code 块 `--mac-bg` 背景 + mono 字体）
- `.drop-hint` — 拖拽导入提示
- `.term-*` / `.msys-*` — 终端信息条（若有），按 toast 信号条风格

每个类家族替换完成后，dev server 里触发一次确认视觉。

- [ ] **Step 6: 全量目视回归**

```bash
npm run dev
```

按 spec §5.3 checklist 过一遍：
- 切换深浅色（此时还没 toggle，用 DevTools 手动 `document.documentElement.classList.toggle('theme-dark')`）
- 新建项目 / 导入项目
- 跑 Stage 1 → 4 完整流程
- 打开每个 dialog / drawer / palette 一次
- Toast 触发各 severity
- DiffView 打开 + 滚动

- [ ] **Step 7: 类型检查 + 单测**

```bash
npm run typecheck
npm run test
```

- [ ] **Step 8: Commit**

```bash
git add src/styles.css
git commit -m "refactor(styles): migrate diff view, review, onboarding, remaining dialogs"
```

---

## Task 7: Theme toggle + localStorage 持久化

**Files:**
- Create: `src/utils/theme.ts`
- Create: `src/utils/theme.test.ts`
- Modify: `src/main.tsx` — 启动前应用持久化主题
- Modify: `src/App.tsx` — topbar 加 toggle 按钮
- Modify: `src/components/CommandPalette.tsx` — 注册 "Toggle theme" 命令

### Task 7.1: `theme.ts` 工具函数（TDD）

- [ ] **Step 1: 写 failing test**

Create `src/utils/theme.test.ts`:

```typescript
import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest'
import { getTheme, setTheme, toggleTheme, applyTheme } from './theme.js'

describe('theme', () => {
  beforeEach(() => {
    localStorage.clear()
    document.documentElement.classList.remove('theme-dark')
  })
  afterEach(() => {
    localStorage.clear()
    document.documentElement.classList.remove('theme-dark')
  })

  describe('getTheme', () => {
    it('returns "light" when no stored value and system is light', () => {
      vi.stubGlobal('matchMedia', () => ({ matches: false }))
      expect(getTheme()).toBe('light')
    })

    it('returns "dark" when no stored value and system prefers dark', () => {
      vi.stubGlobal('matchMedia', () => ({ matches: true }))
      expect(getTheme()).toBe('dark')
    })

    it('returns stored value when present', () => {
      localStorage.setItem('mac.theme', 'dark')
      expect(getTheme()).toBe('dark')
    })

    it('ignores invalid stored value', () => {
      localStorage.setItem('mac.theme', 'garbage')
      vi.stubGlobal('matchMedia', () => ({ matches: false }))
      expect(getTheme()).toBe('light')
    })
  })

  describe('setTheme', () => {
    it('persists the choice to localStorage', () => {
      setTheme('dark')
      expect(localStorage.getItem('mac.theme')).toBe('dark')
    })

    it('adds theme-dark class when dark', () => {
      setTheme('dark')
      expect(document.documentElement.classList.contains('theme-dark')).toBe(true)
    })

    it('removes theme-dark class when light', () => {
      document.documentElement.classList.add('theme-dark')
      setTheme('light')
      expect(document.documentElement.classList.contains('theme-dark')).toBe(false)
    })
  })

  describe('toggleTheme', () => {
    it('switches from light to dark and back', () => {
      setTheme('light')
      expect(toggleTheme()).toBe('dark')
      expect(localStorage.getItem('mac.theme')).toBe('dark')
      expect(toggleTheme()).toBe('light')
      expect(localStorage.getItem('mac.theme')).toBe('light')
    })
  })

  describe('applyTheme', () => {
    it('applies stored theme on startup', () => {
      localStorage.setItem('mac.theme', 'dark')
      applyTheme()
      expect(document.documentElement.classList.contains('theme-dark')).toBe(true)
    })
  })
})
```

- [ ] **Step 2: 运行验证失败**

```bash
npx vitest run src/utils/theme.test.ts
```

Expected: FAIL（模块不存在）。

- [ ] **Step 3: 实现 `src/utils/theme.ts`**

```typescript
export type Theme = 'light' | 'dark'

const STORAGE_KEY = 'mac.theme'
const DARK_CLASS = 'theme-dark'

function isValid(v: string | null): v is Theme {
  return v === 'light' || v === 'dark'
}

function systemPrefersDark(): boolean {
  return typeof matchMedia === 'function' && matchMedia('(prefers-color-scheme: dark)').matches
}

export function getTheme(): Theme {
  const stored = localStorage.getItem(STORAGE_KEY)
  if (isValid(stored)) return stored
  return systemPrefersDark() ? 'dark' : 'light'
}

export function setTheme(theme: Theme): void {
  localStorage.setItem(STORAGE_KEY, theme)
  const root = document.documentElement
  if (theme === 'dark') root.classList.add(DARK_CLASS)
  else root.classList.remove(DARK_CLASS)
}

export function toggleTheme(): Theme {
  const next: Theme = getTheme() === 'dark' ? 'light' : 'dark'
  setTheme(next)
  return next
}

export function applyTheme(): void {
  setTheme(getTheme())
}
```

- [ ] **Step 4: 验证通过**

```bash
npx vitest run src/utils/theme.test.ts
```

Expected: 9 tests pass.

### Task 7.2: 启动时应用主题

- [ ] **Step 5: 修改 `src/main.tsx`**

在现有 `createRoot(...).render(<App />)` 之前，插入 `applyTheme()` 调用。示例（实际代码结构参考现有内容）：

```typescript
import { applyTheme } from './utils/theme.js'

// 在渲染之前立即应用主题，避免首帧闪白
applyTheme()
```

- [ ] **Step 6: 验证**

```bash
npm run dev
```

在 DevTools 里：`localStorage.setItem('mac.theme', 'dark'); location.reload()` — 重载后应用应直接呈暗色，没有首帧白闪。

### Task 7.3: Topbar toggle 按钮

- [ ] **Step 7: 修改 `src/App.tsx`**

在 topbar JSX 里（跟其他 `.topbar-btn` 并列），加入：

```tsx
import { getTheme, toggleTheme } from './utils/theme.js'

// 在 App 组件内部 state：
const [theme, setThemeState] = useState<'light' | 'dark'>(() => getTheme())

const handleToggleTheme = useCallback(() => {
  setThemeState(toggleTheme())
}, [])

// 在 topbar 按钮组中：
<button
  className="topbar-btn"
  onClick={handleToggleTheme}
  title={theme === 'dark' ? '切换到浅色' : '切换到暗色'}
  aria-label="切换主题"
>
  {theme === 'dark' ? '☀' : '☾'}
</button>
```

放在 "⌘K" 按钮旁边。

- [ ] **Step 8: 验证**

```bash
npm run dev
```

点顶栏的 ☾/☀ 图标，整个应用立即切换颜色；刷新后保留上次选择。

### Task 7.4: CommandPalette 注册条目

- [ ] **Step 9: 修改 `src/components/CommandPalette.tsx`**

在现有命令列表里添加一个新 `Command`：

```tsx
import { toggleTheme, getTheme } from '../utils/theme.js'

// 在构建 commands 的地方追加：
{
  id: 'toggle-theme',
  label: getTheme() === 'dark' ? '切换到浅色主题' : '切换到暗色主题',
  shortcut: '',
  section: '外观',
  run: () => {
    toggleTheme()
  }
}
```

（实际 `Command` 类型字段以现有定义为准，只改你需要的几个字段；如果 CommandPalette 的 commands 由 App.tsx 传入，把这条命令加在 App.tsx 的 commands 数组里，紧邻现有 ⌘K 命令附近。）

- [ ] **Step 10: 验证**

```bash
npm run dev
```

按 ⌘K，输入"主题"或"theme"，执行条目，观察切换。

### Task 7.5: 收官

- [ ] **Step 11: 全量 typecheck + test**

```bash
npm run typecheck
npm run test
```

Expected: 全部绿。

- [ ] **Step 12: 手动走完整验收 checklist**

参考 spec §5.3 完整验收：
- [ ] 切换深浅色，所有颜色跟随
- [ ] 从空状态完成 onboarding
- [ ] 新建项目 / 导入项目
- [ ] 跑完 Stage 1 → 4 完整流程
- [ ] 打开每个 dialog（Settings, Templates, Timeline, Doctor, Feedback, Diff, FilePreview, PlanReview, GlobalSearch, CommandPalette, CompletionDrawer, ProjectPicker）
- [ ] Toast 四种 severity 各触发一次
- [ ] ErrorPanel 出现 warn/error
- [ ] DiffView 滚动 + 折叠 + 侧边窄模式

- [ ] **Step 13: Commit**

```bash
git add src/utils/theme.ts src/utils/theme.test.ts src/main.tsx src/App.tsx src/components/CommandPalette.tsx
git commit -m "feat(app): add theme toggle + localStorage persistence"
```

---

## Post-merge

此 plan 产出 7 个 commit。建议以单个 PR 合入 `main`，PR 描述里贴 spec §5.3 的 checklist 让 reviewer 也能按流程过一遍。

## 回滚策略

每个 task 都是独立 commit，若上线后发现问题：
- 视觉回归：revert 对应 commit（例如"drawer 的 hover 变灰太暗"→ revert Task 4 的 commit，其余保留）
- 深浅切换异常：revert Task 7；应用会 stick 在浅色默认值（因为 `:root` 变量始终在）
- 全量回滚：revert 所有 7 个 commit；由于 class 名未改，React 层完全不受影响
