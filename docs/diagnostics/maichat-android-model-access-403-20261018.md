# Android 创作中心模型选择显示错误 403 文案

- 时间与平台：2026-10-18 08:52 CST，OPPO A3x 5G，Android 15，MaiChat 2.2.1（221）。
- 源码与产物：发布标签 `maichat-2.2.1` 指向 `208731a7`；真机安装的签名 APK 为该 Release 中的 `MaiChat-Android-v2.2.1-208731a7.apk`。
- 复现：已登录账号，打开创作中心的「更多 → 模型」，选择 `glm-5.3-flash`。草稿未发送，界面立即显示「登录服务暂时不可用（403）」。App 未崩溃，也没有模型生成请求。
- 调用链：`AIAssistantController.save()` 在缺少 Key 时调用 `MaiChatAuthClient.accountModelKeys()`，请求 `GET /api/v1/model/api-keys`。账号服务的该路径在有效会话下，对不在模型白名单的账号返回 403 和 `Model access is not enabled for this account`。Android `errorText` 未翻译此错误，落入通用「登录服务暂时不可用」分支。
- 线上只读核对：`maichat-auth` 运行中；`/etc/maichat/auth.env` 与运行进程的模型白名单均有一个账号，内容一致。08:50 UTC 建立的唯一有效 Android 会话不在这两个名单中。核对过程未输出手机号或 Key。此 403 是账号授权结果，不是网络故障。
- 客户端修复：将该服务端错误映射为「当前账号未开通模型使用权限」，不改变 HTTP 状态、账号权限或密钥链路。新增对应的 Android 单测，修前 4 项中 1 项失败，修后 4/4 通过。
- 未验证边界：生产白名单尚未修改；是否给这次 OPPO 登录账号开通模型 Key 涉及计费权限，等待用户明确选择。已安装的 2.2.1 APK 不含此文案修复，需后续 Android 产物安装后在同一真机复测。
