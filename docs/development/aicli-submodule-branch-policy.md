# AICLI 子仓分支规范

本仓库只保留一个清晰的维护入口，避免 Codex / OpenCode 子仓分支混乱。

## 固定分支

- 主仓：`main`
- Codex 子仓：`third_party/aicli/codex` 使用 `multi-ai/im-bridge`
- OpenCode 子仓：`third_party/aicli/opencode` 使用 `multi-ai/im-bridge`

`.gitmodules` 已固定两个子仓的跟踪分支为 `multi-ai/im-bridge`。

## 开发规则

1. 主仓开发只在 `main` 上进行。
2. Codex / OpenCode 的 Multi-AI Code 定制改动只提交到各自子仓的 `multi-ai/im-bridge`。
3. 不再用 `fix/*`、`feature/*` 等临时分支承载长期定制功能。
4. 如果为了临时验证创建了实验分支，最终必须把有效提交合入 `multi-ai/im-bridge`，并让主仓 `main` 更新 submodule 指针。
5. 主仓提交 submodule 指针前，必须确认两个子仓对应提交已经推送到远端。

## 日常同步

只想拿到当前主仓锁定的完整版本：

```bash
git checkout main
git pull --rebase origin main
git submodule update --init --recursive
```

需要继续开发子仓定制能力：

```bash
cd third_party/aicli/codex
git switch multi-ai/im-bridge
git pull --rebase origin multi-ai/im-bridge

cd ../opencode
git switch multi-ai/im-bridge
git pull --rebase origin multi-ai/im-bridge
```

完成子仓改动后，先推送子仓，再回到主仓提交 submodule 指针。
