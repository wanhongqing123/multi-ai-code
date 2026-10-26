# 跨设备 Agent 工具审批值不一致（2026-10-24）

- 现场问题：用户在日本用 iPhone 操作位于美国的 Mac Session，询问远端删除工具的逐次批准如何完成。预期是发起操作的 iPhone 显示审批卡；用户点「允许一次」后，Mac Agent 继续执行，所属设备的生命周期删除不再弹第二次确认。
- 复现条件：iOS/Android 远控 Mac Agent Session，Mac Agent 提交需要批准的工具调用，手机在远端会话审批卡点「允许一次」。反向 Mac 远控移动端 Agent 同样受影响。
- 根因证据：iOS/Android 的 Agent 权限桥使用 `approved`、`approved_for_session`、`denied`；原 Desktop 远端面板发送 `approve_once`、`approve_session`、`deny`，原 Desktop owner 只接受这组三值。因此手机→Mac 的权限命令会被 Desktop owner 以「权限决定无效」拒绝，Mac→手机也无法被移动端原生桥接受。协议错配发生在跨设备命令边界，不是距离、网络延迟或目标设备无人值守造成的。
- 修复：统一跨设备 `permission` 命令的 `decision` 为移动端/核心三值。Desktop owner 将统一值映射到已有 `AgentController` 批准/拒绝 API，Desktop 远端面板发送统一值。`maichat_remote_session_delete`、`maichat_remote_session_set_workspace` 的工具定义继续要求逐次批准；审批卡不显示「本会话允许」。生命周期 owner 收到已批准后的命令，仅验证并执行，不二次弹窗。手动列表删除依旧由发起设备做一次破坏性确认。
- 验证：Desktop Host 协议及远端面板用例先复现旧值失败，再改为统一值通过；Desktop Host 22/22、Channel 5/5、Layout 144/144。iOS 模拟器本地 WSS 假 Mac 用例检查远端审批卡显示、无整会话放行按钮、点击后传出 `approved`，1/1 通过。MaiAgent Permission 在相关 8/8 套测试中通过。
- 运行版本：修复已在 Mac 独立测试包 2.2.18 与 iPhone 2.2.3 (112) 源码中；Mac 2.2.18 已启动，iPhone (112) 曾覆盖安装并启动。由于用户随后要求停止操作手机，真实两设备间的 Agent 删除审批与生命周期删除未继续实测，不把模拟器加协议测试称为真机端到端验收。
- 后续模拟器测试账号验证：新增 Agent 远端删除用例，检查批准卡仅有「允许一次」、批准前假 Mac Session 仍在、批准后真实本地 WSS 送达并移除目标 Session，1/1 通过。仍需区分 JS 假 Mac 与完整 Desktop App。
