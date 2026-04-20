# UI Chrome/Material Redesign — Design Spec

**Date:** 2026-04-20
**Scope:** 全量视觉重设计，覆盖主窗口 + 全部 17 个组件。
**Goal:** 把现有"暗色 + 渐变 + 蓝紫强调"的视觉基调，迁移到 Chrome/Material 风格的"浅灰底 + 白卡片 + Google Blue + 细边框 + 极轻阴影"，同时保留暗色主题作为可切换选项。

---

## 1. 范围

- **主窗口**：topbar、4-tile 网格、stage 面板头/体、tile zoom 状态
- **对话框/抽屉（共 13 个）**：CompletionDrawer、DoctorDialog、FeedbackDialog、FilePreviewDialog、GlobalSearchDialog、OnboardingWizard、PlanReviewDialog、ProjectPicker、StageSettingsDialog、TemplatesDialog、TimelineDrawer、DiffViewerDialog、CommandPalette
- **辅助组件**：Toast、ErrorPanel、ReviewChecklist、DiffView
- **样式文件**：`src/styles.css`（2578 行，单文件重写）
- **不在范围内**：
  - React 组件结构 / props / 交互逻辑 — 全部 class 名保持不变，只改 CSS
  - xterm 终端（已有自己的主题系统）
  - 应用打包图标、installer 皮肤

## 2. 设计 tokens

所有颜色、字号、圆角、阴影通过 CSS 变量沉淀在 `:root`（浅色）和 `:root.theme-dark`（暗色覆盖）。组件层只引用变量，不写 literal 值。

### 2.1 颜色（浅色主题）

```css
:root {
  /* Surface（从低到高层级） */
  --mac-bg:            #F1F3F4;  /* 主窗口底 */
  --mac-bg-subtle:     #E8EAED;  /* 次级分隔 */
  --mac-surface:       #FFFFFF;  /* 卡片/面板/dialog 主背景 */
  --mac-surface-raised:#FAFBFC;  /* 面板头/表头等次级 */

  /* 文字层级 */
  --mac-fg:            #202124;
  --mac-fg-muted:      #3C4043;
  --mac-fg-subtle:     #5F6368;
  --mac-fg-disabled:   #80868B;

  /* 强调色（Google Blue） */
  --mac-primary:       #1A73E8;
  --mac-primary-hover: #1967D2;
  --mac-primary-active:#174EA6;
  --mac-primary-soft:  #E8F0FE;  /* 强调色软背景，用于 chip/selected */
  --mac-primary-on:    #FFFFFF;  /* 强调色上的文字 */

  /* 状态色（soft bg + strong fg 成对使用，参考 Material chip） */
  --mac-success:       #1E8E3E;
  --mac-success-soft:  #E6F4EA;
  --mac-danger:        #D93025;
  --mac-danger-soft:   #FCE8E6;
  --mac-warning:       #B06000;
  --mac-warning-soft:  #FEF7E0;
  --mac-info:          #1967D2;
  --mac-info-soft:     #E8F0FE;

  /* 边框 */
  --mac-border:        #DADCE0;
  --mac-border-subtle: #E8EAED;
  --mac-border-strong: #BDC1C6;

  /* Focus ring（强调色半透明外环） */
  --mac-focus-ring:    rgba(26, 115, 232, 0.35);

  /* Diff 专用（语义色，不跨主题大改） */
  --mac-diff-add-bg:   #E6F4EA;
  --mac-diff-add-fg:   #1E8E3E;
  --mac-diff-del-bg:   #FCE8E6;
  --mac-diff-del-fg:   #D93025;
}
```

### 2.2 颜色（暗色主题，`:root.theme-dark` 覆盖）

```css
:root.theme-dark {
  --mac-bg:            #202124;
  --mac-bg-subtle:     #292A2D;
  --mac-surface:       #292A2D;
  --mac-surface-raised:#35363A;

  --mac-fg:            #E8EAED;
  --mac-fg-muted:      #BDC1C6;
  --mac-fg-subtle:     #9AA0A6;
  --mac-fg-disabled:   #5F6368;

  --mac-primary:       #8AB4F8;  /* 暗色用 Chrome 标志性浅蓝 */
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
}
```

### 2.3 字体

```css
:root {
  --mac-font-sans: -apple-system, BlinkMacSystemFont, 'Segoe UI',
                   'PingFang SC', 'Microsoft YaHei', 'Noto Sans CJK SC', sans-serif;
  --mac-font-mono: 'JetBrains Mono', 'Roboto Mono', Menlo, Consolas, monospace;

  --mac-text-xl:   20px;  /* 主窗口大标题 */
  --mac-text-lg:   16px;  /* dialog 标题 */
  --mac-text-md:   14px;  /* 面板头 / 粗体正文 */
  --mac-text-sm:   13px;  /* 常规正文 */
  --mac-text-xs:   12px;  /* 辅助 / 时间 / 状态 */
  --mac-text-mono: 11px;  /* 代码 / 路径 / tickers */

  --mac-weight-regular: 400;
  --mac-weight-medium:  500;
  --mac-weight-bold:    700;

  --mac-line-tight:   1.3;
  --mac-line-normal:  1.5;
  --mac-line-relaxed: 1.7;
}
```

**说明：** 不拉网络字体（Google Sans），用系统字栈对齐 Chrome 观感。等宽栈首选 JetBrains Mono（已在仓库里被引用过）。

### 2.4 形状 + 间距 + 阴影

```css
:root {
  --mac-r-sm:  4px;   /* chip / tag / badge */
  --mac-r-md:  8px;   /* button / input */
  --mac-r-lg:  10px;  /* card / tile / panel */
  --mac-r-xl:  14px;  /* dialog / drawer 外壳 */
  --mac-r-pill:999px;

  --mac-sp-1: 4px;
  --mac-sp-2: 8px;
  --mac-sp-3: 12px;
  --mac-sp-4: 16px;
  --mac-sp-5: 20px;
  --mac-sp-6: 24px;
  --mac-sp-7: 32px;

  --mac-elev-1: 0 1px 2px rgba(60, 64, 67, 0.05);
  --mac-elev-2: 0 2px 6px rgba(60, 64, 67, 0.10);
  --mac-elev-3: 0 8px 24px rgba(60, 64, 67, 0.15);

  --mac-dur-fast:   120ms;
  --mac-dur-normal: 180ms;
  --mac-ease:       cubic-bezier(0.2, 0, 0, 1);
}
```

暗色主题里阴影颜色改为 `rgba(0,0,0,0.3/0.4/0.5)` 对应三档。

## 3. 组件映射

### 3.1 顶栏 `.topbar`
- 背景 `--mac-surface`，下边线 `--mac-border`（去掉渐变和 `::after` 蓝色光条）
- `.topbar h1`：去渐变文本，纯色 `--mac-fg`，`--mac-text-md / 500`；前面加 24px 方形 logo（主色填充 + 白色 "M"）
- `.topbar .meta`：胶囊化——灰底 `--mac-bg`，圆角 `--mac-r-md`，项目名 `--mac-fg` + 路径 mono `--mac-fg-subtle`
- `.meta-warn`：chip 用 `--mac-warning-soft` + `--mac-warning`

### 3.2 顶栏按钮 `.topbar-btn`
- 默认："outlined"：背景 `--mac-surface`、边框 `--mac-border`、文字 `--mac-fg-muted`；hover 背景 `--mac-bg`、边框 `--mac-border-strong`
- `.topbar-btn-primary`：实心 `--mac-primary`、文字 `--mac-primary-on`；hover `--mac-primary-hover`；active `--mac-primary-active`（去渐变、去阴影过重）
- `.topbar-btn-danger`：实心 `--mac-danger`、文字白（保留强信号）
- disabled：`--mac-bg-subtle` + `--mac-fg-disabled` + `cursor:not-allowed`
- 所有按钮加 `:focus-visible { box-shadow: 0 0 0 3px var(--mac-focus-ring); }`

### 3.3 4 阶段 tile
- `.tile`：`--mac-surface` + `1px solid --mac-border` + `--mac-r-lg` + `--mac-elev-1`；hover `--mac-elev-2`
- `.tile-head`：`--mac-surface-raised` 背景、下边线 `--mac-border-subtle`
- 阶段编号：直径 24px 圆形徽章——idle 用 `--mac-bg-subtle / --mac-fg-subtle`，active 用 `--mac-primary-soft / --mac-primary`
- 状态 chip：`running → --mac-success-soft / --mac-success`、`waiting → --mac-bg-subtle / --mac-fg-subtle`、`failed → --mac-danger-soft / --mac-danger`、`done → --mac-info-soft / --mac-info`
- `.tile-zoomed` 保留现有 grid-span 机制，只改视觉变量

### 3.4 Dialog/Drawer/Palette 共用壳
统一到一套 CSS：
- **遮罩**：`rgba(32, 33, 36, 0.5)`（暗色主题 `rgba(0,0,0,0.6)`）
- **容器**：`--mac-surface` + `--mac-r-xl` + `--mac-elev-3`
- **标题区**：padding `--mac-sp-5`，字号 `--mac-text-lg / 500`，关闭按钮用 24px 方形 ghost button
- **正文区**：padding `--mac-sp-5`，`--mac-text-sm`
- **底部 action 区**：靠右排，间距 `--mac-sp-2`，主按钮在最右

**实现方式（不改 React）**：在 CSS 里用 selector 组合，把现有 13 个 dialog / drawer / palette 的根 class 作为一个共用组，统一应用上述规则。示例：

```css
.onboarding-dialog,
.doctor-dialog,
.feedback-dialog,
.stage-settings-dialog,
.templates-dialog,
.plan-review-dialog,
.diff-viewer-dialog,
.file-preview-dialog,
.global-search-dialog,
.project-picker-dialog,
.command-palette,
.completion-drawer,
.timeline-drawer {
  background: var(--mac-surface);
  border-radius: var(--mac-r-xl);
  box-shadow: var(--mac-elev-3);
  /* ... */
}
```

每个组件在这基础上仍可以用自己的 class 补充差异化规则（如 drawer 的滑入方向、palette 的宽度）。

### 3.5 Drawer（CompletionDrawer / TimelineDrawer）
- 从右侧滑入，宽度 420px（保留当前），肩部用 `--mac-elev-3`
- 过渡 `transform var(--mac-dur-normal) var(--mac-ease)`

### 3.6 Toast
- 壳：`--mac-surface-raised` + `--mac-r-md` + `--mac-elev-2`
- 左侧 4px 彩色色条标识 severity：info→primary、success→success、warning→warning、danger→danger
- 自动消失 4s，hover 暂停

### 3.7 CommandPalette
- 居中，宽 640px，`--mac-r-xl`，`--mac-elev-3`
- 搜索框全宽、无边框、仅底部 1px 分割线
- 命令项 hover `--mac-bg`、selected `--mac-primary-soft`；快捷键 chip 用 mono + 边框

### 3.8 输入 / 下拉 / checkbox / radio
- `input[type=text]` / `textarea` / `select`：`--mac-surface`、`--mac-border`、`--mac-r-md`、padding `8px 12px`
- focus：边框 `--mac-primary` + `0 0 0 3px --mac-focus-ring`
- checkbox / radio：用原生 + accent-color: var(--mac-primary)（Chrome 原生样式已足够 Material）

### 3.9 ErrorPanel
- 列表项：每行左侧 16px severity dot（warn→warning、error→danger）
- 行间距紧凑，`--mac-text-xs / mono`；整体滚动容器用 `--mac-surface` + `--mac-border`

### 3.10 DiffView / DiffViewerDialog
- 顶部工具条用 `.mac-dialog-head` 风格
- 代码区：仍用 `--mac-font-mono`；行号列 `--mac-fg-subtle`
- 增/删行用 `--mac-diff-add-bg` / `--mac-diff-del-bg`；行内词级 diff 用对应 fg 加粗
- 滚动条用 Chrome 风细滚动条（webkit scrollbar 自定义）

### 3.11 ReviewChecklist
- 行：hover `--mac-bg`，已勾选项 `text-decoration: line-through; color: --mac-fg-subtle`
- 勾选框沿用 §3.8

### 3.12 OnboardingWizard
- 左侧步骤指示器：竖直排列，完成步骤圆点 `--mac-success`，当前 `--mac-primary`，未达 `--mac-border-strong`
- 右侧内容：`.mac-dialog-body` 规格

## 4. 深浅色切换

- **默认**：浅色（`:root` 生效）
- **切换入口**：两处同时提供——topbar 右端加 toggle（太阳/月亮 icon，持久可见）+ 命令面板里 `Toggle theme` 条目（走键盘）
- **持久化**：`localStorage.setItem('mac.theme', 'light'|'dark')`
- **应用时机**：`main.tsx` 启动时读 localStorage → 在 `<html>` 上加/移 `theme-dark` class；之后直接修改 class 即可，变量自动级联
- **系统跟随（可选扩展，本 spec 不强制）**：首次访问时若无 localStorage 值，按 `matchMedia('(prefers-color-scheme: dark)')` 初始化

## 5. 迁移路径

### 5.1 文件策略
- 仍保留单文件 `src/styles.css`
- 在文件最顶部新增两段 `:root { ... }` / `:root.theme-dark { ... }` tokens（§2 全部）
- 其余规则按组件族分批重写

### 5.2 分批提交（6 个 commit）
1. **feat(styles): introduce --mac-\* tokens & dark theme layer** — 只加变量层，不改任何组件（现有视觉不变）
2. **refactor(styles): migrate topbar + topbar buttons to tokens** — §3.1 / 3.2
3. **refactor(styles): migrate 4-tile grid + stage panel to tokens** — §3.3
4. **refactor(styles): unify dialog/drawer/palette shell** — §3.4 / 3.5 / 3.7
5. **refactor(styles): migrate toast, errorPanel, inputs** — §3.6 / 3.8 / 3.9
6. **refactor(styles): migrate diff view, review checklist, onboarding** — §3.10 / 3.11 / 3.12
7. **feat(app): add theme toggle + localStorage persistence** — §4

每个 commit 在本地 `npm run dev` 下目视验证主要场景不炸。

### 5.3 验收 checklist（手动过一遍）
- [ ] 打开 app，切换深浅色，所有颜色跟随
- [ ] 从空状态完成 onboarding
- [ ] 新建项目 / 导入项目
- [ ] 跑完 Stage 1 → 4 完整流程
- [ ] 每个 dialog 打开一次：Settings、Templates、Timeline、Doctor、Feedback、Diff、FilePreview、PlanReview、GlobalSearch、CommandPalette、CompletionDrawer、ProjectPicker
- [ ] Toast 四种 severity 各触发一次
- [ ] ErrorPanel 出现 warn/error 各一次
- [ ] DiffView 滚动 + 折叠 + 侧边窄模式

### 5.4 风险
- **17 组件一次性 PR**：commit 分批缓解，但最终合并仍是一个大 PR。若过程中主分支有 CSS 冲突需处理。
- **暗色主题现有用户切换回来时字号/布局改变可能不适应**：变量仅换颜色不改几何，影响可控。
- **焦点环样式升级**：`:focus-visible` 在部分 Electron Chromium 版本上 OK，但需验证（Electron 33 已支持）。

## 6. 测试

- **类型检查**：`npm run typecheck` 必须通过（不涉及 TS 改动，预期绿）
- **单测**：`npm run test` 全绿（CSS 改动不影响现有测试）
- **无视觉回归框架**：本次不引入，靠 §5.3 手动 checklist + 截图对比 before/after

## 7. 非目标

- 不做组件库抽取（如 `Button.tsx`）— 保留 class-based CSS 架构
- 不引入 Tailwind / CSS-in-JS
- 不改 React 组件结构与交互逻辑
- 不引入 Google Sans 网络字体（用系统字对齐视觉）
- 不引入视觉回归测试框架
