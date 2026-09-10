# iOS 输入与语音延迟诊断

这些记录用于区分延迟发生的阶段，不代表卡顿根因已定位或速度已改善。

## 语音

`asr / first-text-received` 保留原有 `latency_ms`，并增加：

- `callback_latency_ms`：本轮开始到 SDK delegate 入口的时间，包含录音准备等阶段。
- `callback_extract_ms`：delegate 入口到提取文字并准备投递 MainActor 的时间。
- `main_actor_wait_ms`：投递 MainActor 到开始处理的时间。
- `callback_on_main`：delegate 是否已经在主线程上执行。

`recording-started` 同样记录 delegate 入口时间和 MainActor 等待时间。
delegate 入口不是网络收包时间；如果 SDK 自己先投递到主线程，SDK 内部的排队也会
计入 `callback_latency_ms`。不能看到该值较高就直接归因于网络或识别服务。
`latency_ms` 也不是屏幕实际绘制完成时间。

## 输入框与主线程

`interaction-performance` 下的固定指标只记录次数和耗时，不接收或保存输入正文：

| 事件 | 观测范围 |
| --- | --- |
| `composer-text-mutation` | 应用允许文字变更到收到变更回调，不是物理按键延迟 |
| `composer-edit` | 文字变更回调中的应用处理 |
| `composer-update` | SwiftUI 更新原生输入框 |
| `composer-layout` | 实际执行的输入框尺寸测量，不含缓存命中 |
| `composer-height-queue` | 高度刷新投递到主队列后的等待 |
| `asr-main-actor-wait` | 非空识别回调投递到 MainActor 后的等待 |

高频操作只更新固定大小的内存统计，约每两秒汇总一次；导出和应用失活时也会冲刷
剩余统计。`slow_samples` 的阈值为 16 ms，不等同于测得掉帧。

前台每 250 ms 采样一次主运行循环，仅当额外延迟至少 150 ms 时记录
`main-runloop-delay`，且两秒内最多一次。失活时停止采样，恢复时重置时间锚点。
该记录表示运行循环调度迟到，不提供 CPU 阻塞栈，也不能单独证明某个回调是根因。

## 验证边界

统计窗口的聚合、重置和非法耗时处理有 Core 单测。隔离模拟器探针使用真实的
`AppDiagnosticLog`，注入主线程阻塞和失活/恢复通知，检查日志落盘与采样启停。
该探针不连接 IM、不调用语音服务，不等同于真实手机输入性能或真实后台切换测试。

## 现场记录与启动阶段

[2026-09-10 滚动与语音排障汇总](diagnostics/2026-09-10-ios-performance.md)包含原始日志索引、测量基线、已做改动、未解决问题和下次判断步骤。

语音阶段通过冻结的账号、会话及 `asr_session` 进入远程报告。
`session-started` 的 `audio_session_ms` 拆为 `audio_queue_ms`（串行队列等待）、
`audio_category_ms`（系统录音模式设置）、`audio_activate_ms`（系统激活）及
`audio_resume_ms`（返回 MainActor）。新增 `flow-started`、`recording-stopped`
为 SDK 阶段回调，不代表网络收包时间。
`session-finished.stop_wait_ms` 包括调度等待，`completion=stop-timeout-fallback`
表示使用已有文字兜底；`transcription-applied.duration_ms` 从松手计时，发送模式包含发送等待。
