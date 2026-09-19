# multi-ai-code

这个仓库里有好几个独立的子项目，**各自有各自的规矩**。先认准你在哪个
目录下干活，再去读那个目录的约定。

| 目录 | 是什么 | 约定在哪 |
|---|---|---|
| `MaiAgent/` | agent 核心，纯 C++，可移植的库 | **`MaiAgent/CLAUDE.md`** → `MaiAgent/docs/CodingStyle.md` |
| `MaiChat/` | Qt 桌面端（C++/Qt） | 无独立约定文件；风格照 `MaiChat/desktop/src` 现有代码 |
| `MaiChatBuddy/` | Electron 界面，派生自 opencode（TS） | `MaiChatBuddy/AGENTS.md`（上游带的） |
| `electron/` | 主仓库的 Electron 外壳（TS） | 无独立约定文件 |
| `third_party/aicli/codex/` | codex 源码，**只读参考**，不要改 | 上游的 |

## 动 MaiAgent 的代码

那边的编码规范是硬约束，而且每条背后都有一次真实的代价：

**[`MaiAgent/docs/CodingStyle.md`](MaiAgent/docs/CodingStyle.md)**

开头有一张 21 条的速查表。最容易想当然做错的是这两条：代码里除注释外
不出现中文；碰文件不要用 `std::filesystem`。

## 两个参照系

写代码拿不准的时候去看，不要自己发挥：

- **agent 的形状、协议取值、词汇** → `third_party/aicli/codex/codex-rs`
- **通用基础组件**（路径、文件、线程、字符串、容器） →
  `E:\OpenSource\chromium\src\base`

理由见 `MaiAgent/docs/CodingStyle.md` 的第 0 和 0.5 节——那里记了两次
"自己想当然、结果是错的"的具体经过。

## 通用

- 直接在 `main` 上开发，不要自作主张新建分支。
- 子模块（`MaiChatBuddy/`、`third_party/aicli/`）有自己的提交历史，
  改动要在子模块里单独提交并推送。

---

> `AGENTS.md` 和这个文件内容一致，是给认那个名字的工具看的。
> 两份都只是路由，规则的出处在各子项目自己的约定里。
