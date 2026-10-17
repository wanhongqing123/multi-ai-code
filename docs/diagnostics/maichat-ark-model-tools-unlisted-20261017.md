# Seedance 与 Seedream 模型工具暂停公开

- 时间与平台：2026-10-17；影响 iOS、Android、Desktop 共用的 MaiAgent 工具注册。用户报告方舟账号的 Seedance 相关模型欠费，并要求 Seedance、Seedream 工具下架，使主模型搜不到；工具实现代码保留。欠费状态来自用户报告，本轮没有查询供应商账单。
- 源码基线：`f4a20139936bd6f9fa0bbacd5b0568bf6cfcd025`。原来 Desktop 的 `AgentController` 和移动端的 `MaiMobileAgent` 均把 `seedance_video`、`seedream_image` 注册为模型可见工具。`MaiAgent::pollSpecialistTasks()` 还按工具名查找已提交任务的跟进实现，直接删掉 Seedance 工具会令历史任务失去自动轮询。
- 修复源码提交：`3fd5e74f2a0ec32281edb06ecdf28f06448a1d69`。两个宿主改为内部注册。`MaiToolRegistry::specs()`、`specialists()` 和模型调用入口不包含内部工具；后台按工具名查找的入口仍可轮询已提交的 Seedance 任务。系统提示词移除新建 Seedance/Seedream 任务的指导，保留其他模型工具和方舟素材代码。`services/` 未改动、未部署。
- 验证：MaiAgent 八组相关测试全过，其中假模型测试验证即使模型硬调用 `seedance_video` 也不会执行，通知测试验证内部工具仍可跟进历史任务；Desktop Release 构建及 `agent_controller_test` 通过，假模型收到的工具清单不含 Seedance/Seedream，产品构建启动冒烟通过。iOS Release 2.2.1（89）真机构建、签名校验、安装回读及启动进程检查通过。Android Debug 双 ABI 构建通过；模拟器安装被系统报存储空间不足，两种安装模式均失败，未清理模拟器数据。
- 运行产物：iOS 安装在 iPhone 14 Pro Max，设备回读 `com.kongshang.maichat` 版本 2.2.1、构建 89；构建产物可执行文件 SHA-256 为 `6f596cc2c70e49bef84ad44e2a1a2b0d4ac88ac5c9cb035fa77ae809de0d3fbe`。Mac 候选 App 位于 `MaiChat/desktop/dist-ark-unlist-3fd5e74f/MaiChat-macos-arm64/MaiChat.app`，版本 2.1.2、可执行文件 SHA-256 `454989385fd70c2ec1829660e29fbc86b21cd708c897561c22acb7fcb67e0724`，深度签名验证和启动冒烟通过。当前安装版仍是此前版本，本次未停止或替换；Windows 未构建。
- 边界：Seedance 历史任务的自动状态查询和结果下载保留；主模型不能手动调用已下架工具的 cancel/delete。Seedream 的历史输出文件不删除。移动适配层的单独假模型测试尚未运行，已由 iOS/Android 原生构建和共用核心测试覆盖编译与注册逻辑。旧安装包仍可见旧工具，需安装新客户端才生效。
