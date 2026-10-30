# iOS 阿里云与腾讯云语音识别切换

## 需求与实现

- 2026-10-30，用户要求在 iOS 设置中选择腾讯云或阿里云语音转文字，先完成此功能再处理摄像头自动拍摄。
- 设置入口为「我 → 设置 → 语音 → 语音识别服务」，默认腾讯云。每次长按时固定所选服务，设置变更从下一次识别生效；IM 与创作中心复用同一入口和会话管理。
- 腾讯使用此前已安装并验证过的普通实时识别；阿里使用官方 NUI iOS 2.7.6-01B SDK 和实时语音识别。后台只下发短期 Token，音频由手机直接发送阿里云。
- 两家凭据各自内存缓存，复用账号登录代际、提前 90 秒刷新和迟到请求隔离。登录时预取所选服务；修改选择后下一次长按可按需领取。
- 阿里采集 16 kHz、16-bit、单声道 PCM，SDK 拉取缓存。SDK 操作与音频会话在工作队列串行执行，控制和释放不在 SDK 回调内同步执行；识别任务完成后返回文本，超时或取消不提交部分结果。

## 账号与 SDK

- 官方 SDK 来源、大小、SHA256、架构和文档见 `MaiChat/ios/MaiChat/Vendor/AliyunNUI/UPSTREAM.md`。CocoaPods 下载使用固定 SHA256；包的 XCFramework 包含 arm64 真机及 arm64 模拟器分支。
- 账号服务新增已登录用户接口 `POST /v1/asr/aliyun-credential`，响应为 `app_key`、`token`、`expires_at`；到期时间沿用阿里云 `ExpireTime`，不假定固定时长。腾讯接口保留，音频不经过账号服务。
- 后台配置专用 `MAICHAT_NLS_APP_KEY`、`MAICHAT_NLS_ACCESS_KEY_ID`、`MAICHAT_NLS_ACCESS_KEY_SECRET`；不存在或上游失败时阿里入口返回 503，不影响腾讯或账号服务启动。值不写进源码、日志或本记录。
- 已创建独立 RAM 用户 `maichat-nls-token`，绑定自定义策略 `MaiChatNlsTokenCreateToken`，仅允许 `nls:CreateToken`、资源 `*`；专用配置位于服务器 `/etc/maichat/nls-auth.env`，root 所有且权限 0600。原短信和 OSS 身份权限未改。无密钥操作与回滚凭证 `/private/tmp/maichat-private-deploy-recovery/nls-ram-operation-receipt.json`。

## 验证与边界

- 后台 Token 与 HTTP 定向测试 18/18 通过；实际官方 `CreateToken` 成功，返回过 36 小时有效期，验证不能硬编码 24 小时或腾讯 STS 时长。
- 用户提供的 AppKey 与专用 RAM Token 完成真实 NLS 握手；一次 2 秒合成普通话音频返回中间句、完整“你好，测试成功。”及任务完成，所有状态成功。音频已删除，证据 `/private/tmp/maichat-private-deploy-recovery/aliyun-nls-functional-audio-result.json`。此验证使用真实阿里服务但不代表 iOS SDK 录音已验收。
- iOS SDK 在真实已安装头文件下通过 Swift 6 类型检查；纯状态及 PCM FIFO fixture、两家凭据刷新及账号隔离 fixture 通过。
- 正常 iOS Simulator App 的 `testSpeechProviderSelectionSurvivesRelaunch` 1/1 通过，使用隔离假账号，验证默认腾讯、选阿里、重启后保留和切回腾讯；执行者查看设置页截图 `/private/tmp/maichat-ios143-settings-all-attachments/64A44A2C-AD00-48B5-9EAE-55132866FBC4.png`。
- 生产账号服务及 iPhone 143 安装状态见下方；本地 fixture 不作为真实麦克风录音的证明。

## 生产部署与 iPhone 安装

- 账号服务已部署源码 `319deb6405d1685b4a5c2012985be59ede831a05`，PID `122880`、状态 active，工作目录与 `/proc` 及 `SOURCE_COMMIT` 一致。部署前完成生产 Node 依赖安装、18/18 定向测试与服务身份下的真实 Token 领取。
- 已做 SQLite 在线备份和原 unit/环境配置私有备份；仅新增 systemd `60-nls.conf` 指向本次工作目录并加载 `/etc/maichat/nls-auth.env`，只重启账号服务。原 50 override、账号环境和灰度配置均保留。
- 本机健康 200；公网与本机腾讯、阿里语音凭据接口未登录均 401/no-store，旧签名 URL 接口为 404/no-store。回执 `/private/tmp/maichat-private-deploy-recovery/nls-deployment-receipt.json`；远端备份目录含可移除本次 override 的 `rollback.py`。
- iPhone 真机构建通过，`nuisdk.framework` 仅 arm64，App 深度签名检查通过且旧长期腾讯凭据未进入新二进制。设备回读 `2.2.4 (143)`，安装目录为 `EDF9A91B-EA12-4091-B8F4-F557D71C3D63/MaiChat.app`。
- 安装后设备锁屏导致系统拒绝远程启动（CoreDevice 10002，FBS Locked）；随后设备解锁，已核对本次安装路径的 App 进程 PID 8006。用户随后实际使用并反馈识别错词，见下文。
- 另尝试独立的模拟器原生 SDK driver：编译与链接成功，但 `simctl spawn` 在 `--describe` 阶段被系统以 SIGKILL/137 终止，尚未初始化 SDK；没有领取凭据或发送音频。证据 `/private/tmp/maichat-aliyun-simulator-sdk-probe-20261030.json`。因此不能把直连云端测试和 SDK 类型检查当作 iOS SDK 端到端成功。

## 2026-10-30：「选了」识别为「学了」的限定对照

- 用户报告手机阿里识别结果中的近音词不准确，随后确认原话为「选了」。没有采集、读取或保存用户音频，因此尚不能对原始声音做稳定复现或确定根因。
- 生产源码仍为 `319deb6405d1685b4a5c2012985be59ede831a05`。2026-10-30 09:07:50（Asia/Shanghai），在 macOS 使用本机普通话语音合成「我刚才已经在那选了」，转换为完整 16 kHz、单声道、16-bit little-endian PCM，并保留 0.3 秒尾静音，没有截短发音。
- 同一份音频共 77340 字节、2.416875 秒，SHA256 为 `5cf2fa4bc9579526b900a9e52a25f1a26b30344d0591dd2c62d46b57e2998450`。阿里当前项目与腾讯普通 `16k_zh` 各识别一次，均按实时速率发送全部 13 帧，未循环尝试其他语料或引擎。
- 阿里最终 `SentenceEnd` 为「我刚才已经在那选了。」，随后收到 `TranscriptionCompleted`，所有事件状态为 `20000000`。腾讯最终 `slice_type=2` 同样为「我刚才已经在那选了。」，随后收到 `final=1`，所有回包为 0。原始最终文本完全一致，去除标点后也一致。
- 腾讯中间结果曾短暂返回「我刚才已经在那学」，随后修正为「我刚才已经在那选了」；阿里中间结果也曾缩短后重新展开。这证明本轮中间结果可被后续修正，不能据此断言手机错误来自中间态提交或供应商模型。该合成样本没有复现最终误识别，不能代替手机麦克风、采样转换及停止时序的验收。
- 仅使用上述合成声音，各服务商各一轮，音频和临时脚本已删除；没有输出任何密钥、Token、签名 URL 或用户录音。安全回执 `/private/tmp/maichat-private-deploy-recovery/asr-accuracy-comparison-result.json`，远端当前部署备份目录有同名文件。
- 阿里项目模型只读调查：官方[管理项目](https://help.aliyun.com/zh/isi/user-guide/manage-projects)说明模型由 AppKey 绑定，通过控制台项目功能配置查看及选择，不能在识别请求参数中动态切换。未找到有文档的公开项目模型读取 API，未猜测私有接口，因此当前绑定模型名称尚未核实。
- 使用现有管理员按官方[热词 POP API](https://help.aliyun.com/zh/isi/user-guide/call-the-pop-api-to-manage-business-specific-hotwords)只读调用 `ListAsrVocab` 成功，账号 POP 业务词表总数为 0，已完整覆盖分页。该列表不返回控制台项目自学习绑定，不能推出项目没有名称类或其他控制台热词。可用配置及优先级见[自学习平台 FAQ](https://help.aliyun.com/zh/isi/support/faq-about-self-learning-platform-1)；未创建或修改任何热词、模型或付费设置。查询回执 `/private/tmp/maichat-private-deploy-recovery/aliyun-project-readonly-investigation.json`。


## 2026-10-30 09:02 起：iPhone 143 的“选了 / 学了”准确度反馈

- 现场反馈：用户已在 iPhone 143 实际使用阿里识别，多次把“我刚才已经在那选了”识别为“我刚才已经在那学了”。原话由用户随后确认；未录制或保留用户音频。此反馈发生在上节锁屏阻止启动之后，不能继续把手机状态记为尚未尝试识别。
- 本轮排查基线为 `5a03aadcb13efb0d09f53a8dff724b3d881923cc`，ASR 两个实现文件最后改动来自 `319deb6405d1685b4a5c2012985be59ede831a05`。设备为既有安装的 iOS 2.2.4 (143)，本轮没有重装或修改产品源码、模型及参数。
- 对照配置：阿里和腾讯的宿主代码均设置 `AVAudioSession` 的 `.record + .measurement`，此静态比较不证明腾讯内置录音器后续不会调整会话。阿里将硬件 Float PCM 转成 16 kHz、Int16、单声道再由 NUI 拉取；`sr_format=opus` 是官方支持且默认的传输方式，SDK 内部完成压缩。仅由使用 OPUS 不能推断其造成了错词。实际手机的输入路由、硬件采样率和通道数本轮未取得，不以常见硬件值冒充实测。
- 官方本地 NUI Demo 的 `SpeechTranscriberViewController.m:125` 先将录音缓存置空，再 `nui_dialog_cancel:NO`，然后停录；`STATE_CLOSE/PAUSE` 也停录。当前实现先 `stopCapture()`，再正常结束 SDK。两者存在收尾顺序差异，但 Demo 自身的缓存清理不能用来证明某一顺序必然保留全部尾音。

### 离线 48k/44.1k → 16k 复现实验

环境为 macOS 27.2 arm64、Apple Swift 6.4。只在内存产生两秒合成分段正弦信号，按生产相同的每次 1024 输入帧、同一 AVAudioConverter、相同输出容量与 `.noDataNow` 方式转换。分别测试 48 kHz / 44.1 kHz、单 / 双声道，双声道使用相同信号；不访问麦克风或阿里服务。

- 四种输入最终均可得到 32000 帧、64000 字节、16 kHz / Int16 / mono，零削波。
- 流式转换在明确 endOfStream flush 后，与整段转换逐样本最大差为 0，帧数差为 0；没有发现分块造成的重复、错位或幅度破坏。
- 生产当前不显式 flush 时，四种情况均输出 31994 帧，差 6 个样本，即 0.375 ms。该结果仅证明存在这个微小转换尾差，不能据此把“选 / 学”错词归因于重采样。
- 这是 macOS 的离线 AVAudioConverter 实测，不能替代 iPhone 麦克风、路由与 iOS 实际运行时验证。

复现源码 `Audit.swift` 和结果原件位于本机临时目录 `maichat-aliyun-audio-audit-f8ek8yjn`；安全结果另存 `/private/tmp/maichat-aliyun-converter-audit-20261030.json`。

### 停止时 PCM FIFO 的定量边界

`QueueAudit.swift` 直接编译生产 `AliyunPCMBuffer` 源码，并使用同一 48 kHz 转换输出。以假设的每 40 ms 拉取一次、每次最多 1280 字节，改变四种拉取相位，模拟停止时的数据状态；此调度不冒充 NUI 实际拉取时序。

- 停止时排队 596–1620 字节，相当于 18.625–50.625 ms。
- `stopInput()` 本身没有清空缓存；停止后继续 read，四个场景中全部样本均逐字节保留且顺序正确，无拒写。
- 只有明确 `clear()` 才清空缓存。当前正常 stop 调用 SDK 时没有单独确认应用 FIFO 已排空，且停写后到达的 tap 数据会被忽略；这是需要关注的收尾边界。
- 尚未测得 NUI `nui_dialog_cancel(false)` 之后是否继续拉走手机端的剩余缓存，所以不能把上述“排队字节”写成实际丢失字节，更不能认为已经定位到本次错词根因。

安全结果 `/private/tmp/maichat-aliyun-stop-queue-audit-20261030.json`；总记录 `/private/tmp/maichat-aliyun-audio-audit-20261030.json` 保存源码基线、平台和实验目录。重新运行可在该目录依次编译执行 `Audit.swift`、`QueueAudit.swift`，无需账号、Token、麦克风或识别请求。

### 真实服务对照与本轮结论

后台执行者另经主执行者授权，对同一段 2.416875 秒、16 kHz 单声道合成 PCM 的“我刚才已经在那选了”各调用阿里与腾讯一次。两者最终均正确返回“我刚才已经在那选了。”并明确任务结束；腾讯曾有“在那学”的中间态，随后纠正为“在那选了”。安全凭证 `/private/tmp/maichat-private-deploy-recovery/asr-accuracy-comparison-result.json`。这只证明相同完整合成输入可得到正确最终文本，不证明手机录音无问题，也不代表用户声音或环境的识别效果。

主执行者另只读提取手机日志的时间、事件与非敏感时长：最近三次 voice send 从 `gesture-ended` 到 `transcription-applied` 分别为 501、910、634 ms。说明这三条记录经过 stop 等待，不能直接认定在松手时立即提交了浮层中间态；日志没有 provider、原音及错词绑定信息，也不能证明 SDK 已取完全部应用缓冲，因此不据此排除或确认本次错词原因。

本轮未找到足以归因于客户端采样格式或重采样破坏的证据，未修改采样率、AudioSession 模式、传输编码或模型；没有修复提交，也不承诺调整这些参数即可改善该错词。继续定位需要手机侧不含音频内容的输入路由、输入/输出帧计数、append/read 总字节及 stop 时排队/迟到 tap 计数，才能判定真实供音完整性；本轮停止可选扩测。

依据：[阿里实时 iOS SDK](https://help.aliyun.com/zh/isi/user-guide/nui-sdk-for-ios)、[移动端输入和传输格式](https://help.aliyun.com/zh/isi/user-guide/overview-4)、[Apple TN3136 采样率转换](https://developer.apple.com/documentation/technotes/tn3136-avaudioconverter-performing-sample-rate-conversions)。
