# Windows 创作中心 computer 截屏在 Qt 界面线程失败

- 时间与平台：2026-10-28；Windows 11 10.0.26200，VS2019 x64 Release，Qt 5.15.2。
- 源码基线：`2c12046b14f1b8620d706fe234ed96376e2026fe`，MaiChat Desktop 2.2.31。
- 运行对象：独立构建目录 `MaiChat/desktop/build-release-v0.1.95` 的
  `desktop_computer_tools_test.exe`；没有运行安装器或替换用户现装 App。

## 复现与根因

在真实可见的 Qt 测试窗口上运行 `desktop_computer_tools`，执行 `observe` 时返回
`Windows Graphics Capture failed (0x80010106)`，测试无法拿到截图。
`0x80010106` 是 `RPC_E_CHANGED_MODE`。`MaiScreenshotWindows.cpp` 的显示器与窗口路径
都无条件执行 `winrt::init_apartment(multi_threaded)`，而 Qt/OLE 界面线程已经是 STA。
控制台截屏测试通常运行在未初始化 COM 的线程，因此此前没有暴露此冲突。

第一次修法在每次截图后直接 `uninit_apartment()`，让 Qt 用例通过，却使控制台测试的
“先截显示器、再截窗口”在第二次 `GraphicsCaptureSession::IsSupported()` 崩溃。
Windows 调试器定位到该 WinRT 工厂虚函数调用访问失效地址；C++/WinRT 头文件中的
工厂缓存会保留引用，释放最后一个 COM apartment 后不能直接复用缓存。
因此最终实现先清理 `winrt::clear_factory_cache()`，再配对释放本次 COM 初始化。

`DesktopComputerToolsTest` 还发现输入测试自身的事件投递问题：`SendInput` 成功且
输入框取得焦点后，`QThread::msleep()` 加单次 `processEvents()` 不一定处理完字符事件。
测试改为运行短时 Qt 事件循环，再断言输入文本；产品点击和键入实现没有改动。

## 修复与验证

修复提交：本文所在提交。截屏时先尝试 MTA，仅在模式冲突时沿用 STA；作用域结束前清理
WinRT 工厂缓存，再调用 `uninit_apartment()`。显示器与窗口入口复用同一实现。

- 在同一 `2c12046b` 基线上临时撤去产品修复，`desktop_computer_tools` 稳定失败并输出
  `0x80010106`；恢复修复后同一测试通过，并连续运行 3 次通过。
- 亲看测试输出的 504×367 PNG：窗口标题、`Waiting` 标签、输入框和 `Confirm` 按钮
  清晰可见，无黑屏或空白；点击、键入 `computer OK` 的用例断言通过。
- 干净重链独立 `MaiScreenshotToolTests` 后，显示器和窗口连续截屏通过；该测试单独重复
  3 次通过。此前独立测试缓存有与新 `MaiTool.h` 不一致的旧对象文件，已干净重链排除。
- MaiAgent 核心、模型客户端、TurnRunner、工具、工具循环、权限、存储、截屏 8/8
  CTest 通过。`MaiStoreTests` 在 MSVC 下需要显式捕获局部常量 `kPerThread`，本提交同时
  修正这一处测试编译兼容性。
- Desktop Host、process、browser、computer、AgentController、面板、登录、布局
  8/8 CTest 通过。`maichat` Release 构建后验证 90 项 PE 导入与 Qt 插件。
- Desktop CMakeCache 回读 FFmpeg、OpenCV、远程桌面均为 ON；找到 Qt5WebSockets
  CMake Config，构建目录内有 `Qt5WebSockets.dll`。
- 构建目录中的 `maichat.exe --smoke` 进程退出码为 0，落盘日志回读版本 2.2.31、
  Qt 5.15.2 和实际可执行文件路径。此处不是最终安装器或 ZIP 的隔离验收。

边界：这里验证的是 Windows 测试窗口及本机显示器，没有执行用户的真实付费任务。
正式 EXE/ZIP 的独立暂存、解压后裸 PATH 烟测、哈希与上传需在最终冻结提交后完成。
