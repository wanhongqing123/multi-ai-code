# Codex 应用专用共享目录

Multi-AI Code 启动的内置 Codex 使用 `<数据根>/.codex`，与 `accounts/` 平级。
数据根默认是 `~/multi-ai-code`，也可通过 `MULTI_AI_ROOT` 指定。
主终端与仓库分析终端都在共用的 PTY 启动层设置 `CODEX_HOME`、
`CODEX_SQLITE_HOME`，不以账号、项目路径或临时 sessionId 命名。
所有应用账号共享 Codex 登录、配置和会话历史；其他账号数据仍然隔离。

- 不使用宿主机全局 `~/.codex` 及其子目录，不在打开的项目仓库内创建 Codex 状态目录。
  若自定义数据根或符号链接使共享目录落到这些位置，启动前明确报错，不写入该目录。
- 不复制 auth.json、配置、技能、会话、数据库或锁文件。首次启用独立目录时，
  需要在共享目录内重新登录并配置 Codex；旧账号目录原样保留，不自动搬运或合并。
- 未登录应用账号仍不允许启动 Codex。继承的 CODEX_HOME / CODEX_SQLITE_HOME
  （含 Windows 大小写别名）不能覆盖应用选择的共享目录。
  目录创建失败就停止启动，不静默回退到全局目录。
- 清除继承的调试用 CODEX_ANALYTICS_EVENTS_CAPTURE_FILE，避免向其他目录写采集文件；
  不清除企业托管配置或 CA 证书变量，不绕过管理策略。
- 正常的项目级 `.codex/config.toml` 读取规则仍由 Codex 决定，本功能不删除现有项目配置。
- 排障日志导出的 `codexHome` 是宿主为进程设置的共享目录。
  Codex 显式配置的 `sqlite_home` 等其他路径仍由内核解析；宿主不改写用户配置。

这是宿主启动配置修改，不需要改 Codex 内核。升级后须重新启动 Codex 进程，
已在运行的进程不会切换目录。共享存储不绕过内核的单会话写锁：
两个账号同时恢复同一个活跃会话仍会被拒绝，不能当作并发写入同一会话的功能。
不把本改动作为此前 Windows `0xC000013A` 退出的已确认根因修复。

官方配置说明：<https://developers.openai.com/codex/config-advanced/>
