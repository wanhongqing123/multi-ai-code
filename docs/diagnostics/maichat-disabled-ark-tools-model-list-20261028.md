# 已停用方舟工具仍占用模型上下文

- 现场：2026-10-28，用户在 macOS Desktop 创作中心询问当前工具，回复仍列出因余额不足停用的 Seedance / Seedream，并列出只服务于该流程的 `ark_assets`。现场截图来自已发布的 2.2.31；修复前源码基线为 `3446aefaa14315007b206f6f71030b0d732bfd74`。
- 根因证据：Desktop `AgentController.cpp` 和移动端 `MaiMobileAgent.cpp` 已通过 `addInternal` 隐藏 Seedance / Seedream 工具定义，却仍在基础指令中主动写入这些名称；`ark_assets` 则使用 `add` 注册，仍进入模型工具清单。后台轮询旧 Seedance 任务不需要把上述工具暴露给主模型。
- 修复：Desktop 与移动端把 `ark_assets` 改为内部注册，并删除基础指令中针对停用方舟流程的内容；保留旧任务后台查询的内部实例和普通代码接口。相关模型请求测试同时检查基础指令和工具定义不含这三项。
- 验证：macOS Desktop `agent_controller_test` 1/1 通过（9.36 秒）；移动端 `MaiMobileAgentTests` 在 iOS 26.5 和 27.1 ARM64 模拟器均通过，覆盖工具请求、权限、持久化和模型可见清单。测试中同时修正了原先写死 Android 相机工具名的 iOS 用例。
- 边界：历史对话已经保存的文本仍可能提到这些名称；已发布 App 未更新，本次没有 Windows 构建或新 Release。修复提交见本文件的 Git 历史。
