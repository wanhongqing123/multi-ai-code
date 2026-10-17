# MaiChat Desktop 创作中心模型切换与顶栏不一致

- 报告时间与平台：2026-10-17，Windows 10/11、MaiChat Desktop 2.1.2。用户截图显示创作中心顶栏同时展示模型、权限和历史字数，点击模型配置又打开独立弹窗；iOS 基准是固定的“创作中心”标题，模型和权限在右上更多面板内展开。
- 源码基线：`3bad83a066b93606c586b92da01f0485192e859a`。现场复现：进入创作中心，在顶栏查看模型/权限/字数，再进入模型配置；在已有会话中选择其他供应商，观察会话与草稿。新增回归用例还复现了首次创建会话前切换模型失败。
- 根因证据：`AgentChatPanel` 把三个状态控件直接加入顶栏；`MainWindow::editAgentModelSettings()` 以模态对话框选择模型，保存后调用 `rebuildAgentPage()` 删除并重建会话面板与控制器。旧控制器在构造时固定模型 URL 和 Key，因此切换模型依赖重建，导致视图状态有丢失风险。首次会话前 `AgentController::setModel()` 拒绝空 session ID，切换也无法完成。
- 修复提交：`c10284fadedcec9284b7ca9902d4a7cb45a9e773`。顶栏固定居中标题，只保留右侧更多；更多面板原位展开三种模型、操作权限和清空当前对话。模型切换前刷新账号 Key，在同一个控制器中按所选模型直连对应供应商；Key 在请求时从线程安全的内存状态读取。切换失败保留原模型、会话、草稿及滚动位置；首次会话前可选模型，新会话继承该选择。
- Windows 验证：修复前的新“首次会话前切换”用例失败，修复后通过。VS2019 x64 Release 增量构建 `maichat` 成功；`agent_controller_test`、`agent_panel_session_test`、`main_window_layout_test`、`login_dialog_test` 四组 CTest 全过（约 55 秒）；`npm run check:line-width` 与 `git diff --check` 通过。实际 Qt 渲染图在 100% 和 150% 缩放下检查了固定标题、更多面板展开与控件裁切。当前构建路径的 `maichat.exe --smoke` 启动，日志版本为 2.1.2。
- 边界：本次未向付费模型供应商发起请求，也未重新打包发布。Mac 原生 Cocoa 界面与交互由另一端基于本提交复核；已发布的 2.1.2 安装包不包含此修复。
