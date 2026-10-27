# Session 定时 Agent 任务与系统日历／闹钟（设计草案）

## 用户目标

- 在某个 Agent Session 中用工具创建、列出、取消定时任务；到点自动将预设
  指令提交给该 Session。iPhone、Android、Mac、Windows 都能管理任务。
- 可在系统日历或闹钟中看到相应提醒。系统提醒与 Agent 的实际执行状态
  必须分开显示，不能把提醒响起报告为 Agent 已完成。

## 平台事实与执行边界

- iOS `BGTaskRequest.earliestBeginDate` 只指定不得早于该时间，系统不保证
  准点启动；用户强制关闭 App 后，后台推送也不会自动启动它。因此不能承诺
  iPhone 本机 Agent 在锁屏、强退后准点执行。依据：
  [Apple Background Tasks](https://developer.apple.com/documentation/backgroundtasks/bgtaskrequest/earliestbegindate)、
  [Apple remote notification delegate](https://developer.apple.com/documentation/UIKit/UIApplicationDelegate/application%28_%3AdidReceiveRemoteNotification%3AfetchCompletionHandler%3A%29)。
- Android 可使用系统精确闹钟，但后台执行时长、耗电策略和网络仍受系统
  约束；闹钟触发不等于一轮模型任务完成。依据：
  [Android alarms](https://developer.android.com/develop/background-work/services/alarms)、
  [AlarmManager](https://developer.android.com/reference/android/app/AlarmManager)。
- Mac／Windows 若作为常开 Session 所属设备且 App 在运行，可按本机时钟
  提交 Agent 指令；电脑关机、睡眠或 App 未运行时也无法承诺准点。可配置
  OS 登录启动和唤醒后补跑，但不能把它写成“所有情况下精确执行”。
- MaiChat 账号服务只保存控制面或传递任务状态，不代理模型请求和媒体字节；
  Agent 始终在 Session 所属设备上执行。

## 共用任务契约

- 任务固定绑定 `account_id`、所属设备、`session_id`，包含唯一任务 ID、
  指令、明确时区的执行时间、创建时间及状态。先做一次性任务；重复规则
  须在本地日历时区计算下一次，不能只加固定 24 小时来处理夏令时。
- 状态区分 `scheduled`、`started`、`completed`、`failed`、`uncertain`、
  `canceled`。同一任务只能被一个执行者原子领取；崩溃落在“已领取但尚未
  持久记录 Agent 消息 ID”的窗口时标为 `uncertain`，不自动重复付费调用。
- 创建／改期／取消 Agent 工具均显示完整任务内容并逐次审批。创建时批准
  未来自动提交这一条指令；该轮 Agent 后续如要求工具审批，仍须独立批准，
  不能继承创建定时任务的许可。
- 系统日历事件和闹钟是可选提醒镜像，由各平台宿主按用户授权操作；
  它们不是调度任务的唯一真相。创建成功须返回系统条目 ID 或明确说明
  需要用户在系统 App 中完成导入。

## 待用户确定

- 执行设备离线、手机 App 被系统挂起或强退时：恢复后补执行一次，还是
  视为错过；若要求手机本机准点执行，iOS 平台不具备可保证的系统机制。
- 是否接受常开 Mac／Windows 承担需要准点的 Session 任务，手机端负责
  创建／管理和系统提醒。
