# Codex 账号目录隔离

Multi-AI Code 启动的内置 Codex 使用 `<数据根>/accounts/<账号>/.codex`。
主终端与仓库分析终端都在共用的 PTY 启动层设置 `CODEX_HOME`、
`CODEX_SQLITE_HOME`，不以项目路径或临时 sessionId 命名。不同账号打开同一仓库
时使用不同状态目录，同一账号重启后保留自己的历史。

- 不创建或迁移项目仓库里的 `.codex`，也不自动导入客户全局 `~/.codex`。
- 不复制 auth.json、配置、技能、会话、数据库或锁文件。首次启用独立目录时，
  需要在该实例内登录并配置 Codex；旧历史仍在原目录，不会自动出现在新实例中。
- 继承的 CODEX_HOME / CODEX_SQLITE_HOME（含 Windows 大小写别名）不能覆盖账号目录。
  目录创建失败就停止启动，不静默回退到全局目录。
- 清除继承的调试用 CODEX_ANALYTICS_EVENTS_CAPTURE_FILE，避免向其他目录写采集文件；
  不清除企业托管配置或 CA 证书变量，不绕过管理策略。
- 正常的项目级 `.codex/config.toml` 读取规则仍由 Codex 决定，本功能不删除现有项目配置。
- 排障日志导出的 `codexHome` 是进程实际使用的账号目录。

这是宿主启动配置修改，不需要改 Codex 内核。它隔离不同账号的默认会话发现与存储，
不绕过内核的单会话写锁，也不保证同一账号内重复恢复同一个活跃会话会成功。
不把本改动作为此前 Windows `0xC000013A` 退出的已确认根因修复。

官方配置说明：<https://developers.openai.com/codex/config-advanced/>
