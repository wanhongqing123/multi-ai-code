# Session 定时 Agent 任务与系统日历

## 用户目标

- 在某个 Agent Session 中用工具创建、列出、取消定时任务；到点自动将预设
  指令提交给该 Session。iPhone、Android、Mac、Windows 都能管理任务。
- 可在系统日历中看到相应提醒。系统提醒与 Agent 的实际执行状态
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

- 任务保存在当前账号的本机 Session 数据库旁，固定绑定本机所属设备与
  `session_id`，包含唯一任务 ID、指令、明确时区的执行时间、创建时间及状态。
  当前仅支持一次性任务，不提供重复规则或跨设备迁移。
- 状态区分 `scheduled`、`started`、`completed`、`failed`、`uncertain`、
  `canceled`。同一任务只能被一个执行者原子领取；崩溃落在“已领取但尚未
  持久记录 Agent 消息 ID”的窗口时标为 `uncertain`，不自动重复付费调用。
- 创建／取消 Agent 工具均显示完整任务内容并逐次审批。创建时批准
  未来自动提交这一条指令；该轮 Agent 后续如要求工具审批，仍须独立批准，
  不能继承创建定时任务的许可。
- 系统日历事件是可选提醒，由各平台宿主按用户授权操作；
  它们不是调度任务的唯一真相。创建成功须返回系统条目 ID 或明确说明
  需要用户在系统 App 中完成导入。

## 已确认的执行方式与当前实现

- 2026-10-27 用户同意先用常开 Mac／Windows 承担需要准点的 Session
  任务。四端均能在本机 Session 创建、列出、取消任务；手机打开跨设备 Session
  时，创建、查看和取消指令发到所属电脑的 Agent，由电脑本地保存和执行。
  手机 App 可运行时也会执行手机本机任务，但锁屏挂起或强退时不承诺准点。
- 任务到点后由 Session 所属设备提交一次 Agent 指令。设备重启、离线或 App
  停止后，恢复运行时在 24 小时窗口内补跑；超过窗口标为 `missed`。
  崩溃后无法判定提交结果的任务标为 `uncertain`，不会自动重试。
- `maichat_session_task_create/list/cancel` 使用同一份可移植 C++ 存储和调度器；
  时间必须为带 `Z` 或 UTC 偏移的 ISO 8601 字符串。运行任务时仍使用原有
  模型与工具审批，创建任务时的审批不会转成对后续工具调用的批准。
- `maichat_calendar_create_event` 是独立的日历入口。iOS 用 EventKit、Android
  用 CalendarContract 经系统权限写入事件；Mac／Windows 生成 `.ics` 并打开
  默认日历导入器，只有用户在日历中完成导入后才算保存。日历提醒不会自动
  创建或代替 Agent 定时任务；目前没有闹钟写入入口。手机在远端电脑 Session
  中调用日历工具会操作电脑日历；要写入手机日历，需在手机本机 Session 调用。
- 当前任务只保存在 Session 所属设备上，没有任务数据库的账号云复制，也没有
  跨设备接管。手机创建远端电脑任务时，所属电脑必须在线接收指令；创建成功后，
  手机离线不影响电脑执行。电脑睡眠、关机或 App 退出时无法准点执行，需保持
  App 运行、设备唤醒且联网。
