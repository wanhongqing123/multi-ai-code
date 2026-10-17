# Windows FFplay 连续打开视频时崩溃

- 时间与平台：2026-10-18，Windows x64、NVIDIA RTX 5080 Laptop GPU、MaiChat Desktop 2.2.1 候选源码 `574be43d5caf0559a9cfb040553c058bda83ee98`。发布前运行 `MaiFfplayVideoDialogTest`，先打开并关闭有 AAC 音轨的 MKV，接着播放 H.264 时进程以 `0xc0000005` 退出；单独运行 H.264 用例通过。同一进程的两段视频用例在 CTest 下可稳定复现。
- 定点证据：崩溃地址 `0x00007FFC476F2A00` 位于该进程加载的 `wdmaud.drv` 地址范围内，不能仅凭附近的 D3D11 初始化日志归因于显卡。Windows WinMM 适配的 `SDL_CloseAudioDevice` 原先在音频写入线程退出前调用 `waveOutReset`，两者会并发操作同一输出设备。新增的视频测试还断言了仅在 macOS 分支创建的全屏按钮，Windows 单独运行停在该错误断言。
- 修复提交：`c78fb325456eab9d261bc88d72ddb822bace27ee`。WinMM 关闭流程先唤醒并等待音频写入线程退出，再调用 `waveOutReset`、释放缓冲和关闭设备；视频测试仅在 macOS 断言并操作该平台的全屏按钮。两处变更均不修改 macOS/iOS/Android 产品代码路径。
- 红绿验证：修复前 `MaiFfplayVideoDialogTest` 连续 CTest 运行多次崩溃；修复后该整套测试连续 5 次通过。最终 Windows VS2019 Release 产品增量构建成功，FFplay 音频、视频对话框、创作中心、登录、文件和视频预览等 10 组 CTest 全部通过。
- 边界：这证明本机测试样本的连续播放、控制与关闭路径不再触发该崩溃，尚未覆盖所有驱动和真实聊天中的全部视频格式。已发布安装包未替换；应从包含本修复的提交重新打包并在独立路径验证。
