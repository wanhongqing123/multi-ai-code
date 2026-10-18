# Android 创作中心退出时的 Agent 后台线程崩溃

- 时间与平台：2026-10-18 09:36 CST，Mac 上的 Android 37 arm64 模拟器，MaiChat 2.2.2 Debug，源码 `3ce0cbad`。
- 稳定复现：同一个 instrumentation 进程中依次运行 `tappingConversationDismissesComposerFocusAndKeepsDraft` 和 `keyboardSendActionSubmitsTheDraft`，首项通过，第二项之后进程崩溃；两项各自放入新测试进程均通过。与此前 7 项 ASR 设备测试连跑时同样崩溃。
- 原始观察：`mai-turn` 线程触发 SIGSEGV，fault address `0xc8`。当前设备的 `libmaichat_agent.so` BuildId 为 `ac382ba2c2582de2b21c03d9469c11e71cc0d2e0`；符号化到 `MaiAgent.cpp:246` 的 `Runtime::retire` 调用 `MaiEventEmitter::emitSession`。反汇编显示读取 emitter 的 `mBus` 引用时 emitter `this=0xc8`，对应 `MaiAgent::mRuntime` 已置空后的成员偏移。
- 根因：旧代码只在 `Runtime` 析构体里停止并等待对话线程；`MaiAgent` 的 `unique_ptr<Runtime>` 在进入该析构体前已经清空，后台线程仍通过 `MaiAgent::mRuntime` 调用 `retire`。此外 `retire` 会先把自身线程从 `active` 表移除并 detach，然后继续通知条件变量，仅等待 `active` 中线程无法覆盖这段尾声。
- 修复：在 `MaiAgent` 析构体中、`mRuntime` 仍有效时先取消并等待所有 turn 工作线程；用受互斥锁保护的在途线程计数包含已从 `active` 移除的尾声，等待计数归零才释放 Runtime。保留 Runtime 自身的幂等收尾，并唤醒等待用户问答的线程。
- 验证：同进程两项设备测试由崩溃变为 2/2 通过；与 7 项录音/WebSocket 设备测试连跑为 9/9 通过。MaiAgent 八套核心 CTest 8/8 通过。OPPO 当前仍在运行旧 2.2.1 视频任务，尚未安装包含修复的 APK，真机退出/重进场景未验证。
