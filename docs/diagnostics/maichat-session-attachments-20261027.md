# 创作中心跨设备会话附件与云盘入口（2026-10-27）

## 现场和预期

- 用户在 iPhone 创作中心的跨设备会话看到“跨设备会话暂只支持文字消息”；
  图片、视频、音频、文件不能随会话在设备之间使用。
- 创作中心的初始介绍列出图片创作等入口，却没有云盘。
- 预期：会话成员选中图片、视频、音频或文件后上传到独立的会话附件空间；
  所属设备下载到工作区后再交给 Agent；同账号的会话历史显示附件并能下载；
  个人云盘目录与配额不受影响；创作中心明确展示云盘入口。
- 复现平台：用户提供的 iPhone 截图及 2026-10-27 发布版 2.2.29。
  开发基线为 `a5e48f4b8`，Mac、Android 和 iOS 的工作构建见下文。

## 证据与原因

- iOS、Android、Desktop 的远端 Session 发送链只传文字；远端选择附件入口
  被禁用或显示上述提示，Session Sync 命令中没有可持久引用的附件 ID。
- 既有私人云盘 OSS 只能供个人云盘媒体使用；服务端元数据、配额、对象前缀
  都归属云盘。因此直接把跨设备附件放入个人云盘会混合两种产品空间。
- 创作中心欢迎页的固定入口列表没有“我的云盘”和“跨设备会话”卡片。

## 改动和验证

- 复用现有可移植 C++ HTTP 上传下载内核，新增独立的 Session 附件路径；
  服务端使用 `session_attachments` 元数据表、`session-attachments/<账号>/`
  OSS 对象前缀和独立 10 GB 默认配额，按账号及所属设备槽位、Session ID
  校验下载回执。支持四类附件，图片最大 20 MB，其他最大 2 GB。
- 远端发送、所属设备接收和工作区落盘、会话历史下载、失败后草稿恢复，已接入
  iOS、Android 与桌面端；Agent 有与普通下载共用实现的只读工具入口。
- iOS 初始页加入“我的云盘”和“跨设备会话”；Android 初始介绍补充云盘。
- 2026-10-27 本地验证：Auth `npm test` 152/152；OSS 签名端
  `npm test` 61/61；RAM 升级脚本 2/2；生命周期脚本 4/4；
  iOS 模拟器 Debug、Android Debug/AndroidTest、Mac Desktop 构建成功；
  Mac Host 22/22、Agent 工具及本地 TLS 通过；Android 模拟器远端附件
  下载动作测试 1/1；可移植 C++ 附件本地 TLS 1/1（37.40 秒）。
  最终 iOS 模拟器欢迎页截图已人工检查，12 个卡片与输入框无遮挡。
- 还须以最终提交哈希复核 Windows VS2019/Qt 构建及真实跨设备、真实 OSS
  上传下载。

## Windows 附件测试编译补正

- Windows 在 `ff63b9d70` 上使用 VS2019/Qt Release 重链主程序及 Desktop
  Host、Agent、布局、云盘工具和对话框目标均成功。单独启用 Network 测试后，
  `MaiChat/Network/tests/MaiCloudDriveClientTest.cpp:133` 的 `std::min(...)`
  被 Windows `min` 宏展开，MSVC 报 C2589/C2062/C2059；产品源码未受影响。
- 将此处调用写成 `(std::min)(...)`，消除宏展开，同时保持原有最小值逻辑。
  Mac 测试目标重链成功，本地 TLS 回归 1/1 通过（37.41 秒）；Windows
  仍需从修复哈希在不加 `CL=/DNOMINMAX` 的环境下重新验证。
- Windows 从 `6ac40c9ac` 不加诊断编译参数后，Network 测试已编译链接；
  本地 TLS 在取消上传后的“restart task list”断言失败。临时布尔诊断显示
  状态 OK、名称匹配、两个凭据字段均未暴露，只有原始 JSON 文本查找
  `filePath` 为 false。Windows 路径分隔符在 JSON 中需转义，测试的
  字符串子串判断不适用。测试现解析 `uploads[0].source_id`，比较解码后
  的路径，并保留原凭据未泄露断言。Mac 重链、本地 TLS 1/1 通过
  （37.41 秒）；真实附件用例仍待 Windows 复跑。

## 上线边界

- 生产 ECS 尚未改动。Mac、Windows 对 `ichat.life` 当前 SSH 主机密钥没有
  可信的既存记录；Windows `ssh-keyscan` 仅给出未认证网络观察，严格主机
  密钥验证拒绝登录。须从可信渠道拿到生产 ED25519 指纹与授权登录方式，
  再执行 RAM 策略扩展、OSS 生命周期规则、服务端部署和真实 OSS canary。
- iPhone 真机当前未连接；不能把模拟器构建视为真机验收。现用 App 未替换。
- 新客户端依赖服务端和 OSS 权限先就绪。在生产验证前不能称此功能已上线。
