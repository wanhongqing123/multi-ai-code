# 云盘视频缩略图倒立（2026-10-26）

## 现场与复现

- 平台：用户的 iPhone 14 Pro Max，MaiChat 云盘页；本轮源码基线 `7e74501987f3483b8e2a32ebfe44a17f6c2a2d23`。截图中第三个视频 `gallery-0283...0B951620.mov` 的缩略图上下颠倒，同页其他视频缩略图正立。截图没有提供 App 构建号，本轮未把截图归因到某个安装产物。
- 用户此前提供的同场景原始 MOV 为 18,452,323 字节，SHA-256 `0d5c2ac9f84d9502ec489f7e34398aaae1ac2aae800941a814ba59e47ee7be14`。第三行显示 18.5 MB、同场景倒立帧。部署后只读查询生产云盘元数据，以截图中的文件名后缀和精确大小唯一定位一条 `ready` 视频；生产 signer 对此旧对象返回的 JPEG 与原片测试副本经同一参数截出的正立 JPEG SHA-256 完全一致。未对生产对象做全文件哈希回读，因此不宣称两者全部字节相同。
- `ffprobe` 显示该文件视频轨含 Display Matrix `rotation=-180`。同文件还含 HEVC Main 10、HLG、Dolby Vision profile 8 与五条 `mebx` data 轨；这些信息无法单独解释同组视频中只有此条缩略图倒立。
- 对同一原片 0.5 秒帧分别执行 FFmpeg 默认自动旋转与 `-noautorotate` 截图。人工查看结果：默认输出正立；不旋转输出倒立，与用户截图第三行方向一致。两张 320×180 JPEG 的 RGB 均方误差：默认输出对“不旋转再转 180°”为 17.43；对“不旋转原样”为 3969.99。JPEG 重编码造成小误差。

## 调用链与原因

1. 三端云盘均向账号服务请求本人视频的 `thumbnail-link`；账号服务向媒体签名器取私有 OSS 处理 URL，不经服务端转发图片字节。
2. `services/oss-media-signer/index.js` 原来对所有视频签 `video/snapshot,t_0,f_jpg,w_320`。该处理未要求 OSS 按视频的旋转信息调整截帧；iOS 只解码返回的 JPEG，不再读取原 MOV 的 Display Matrix。Android 和 Desktop 也是直接显示所得 JPEG。
3. [阿里云 OSS 视频截帧文档](https://help.aliyun.com/zh/oss/video-snapshots/)定义 `ar_auto` 为根据视频信息自动旋转截图；[参数错误说明](https://help.aliyun.com/en/oss/user-guide/0053-00000108)也列出有效值和 `ar_auto` URL 示例。原片真实 OSS A/B 进一步验证了“默认截帧未应用 Display Matrix”这一定位。

## 修复与验收

- 统一签名器的视频处理参数改为 `video/snapshot,t_0,f_jpg,w_320,ar_auto`。参数在 V4 签名时加入，不由客户端在签名后追加；图片的处理参数保持原样。同步更新签名参数测试、真实 ali-oss SDK 的视频 URL 签名断言、一次性 OSS 缩略图探针预期值和设计文档。
- `services/oss-media-signer` 全部 56/56 本地测试通过，相关签名与探针定向测试 12/12 通过。FFmpeg 原片本地自动旋转截图已人工查看。修复提交 `77b72c4683d30e11bcb2303bd2da1fbb228e0309` 已推送 `main`。
- 在云盘专用私有 Bucket 的随机隔离测试账号前缀，先上传人工生成的 2,359 B H.264 MP4：视频像素上半红、下半蓝，Display Matrix `-180`。签旧参数和 `ar_auto` 两个 60 秒短链取 JPEG：旧图红上蓝下，新图蓝上红下；图像 RGB 均方误差（旧对新原样 / 旧对新转 180°）为 42498.06 / 12.72。测试对象删除并 HEAD 404，恢复日志清。
- 再用用户为排障提供的 18,452,323 B 原片走相同的一次性测试路径。旧参数返回 26,568 B JPEG，肉眼见倒立；`ar_auto` 返回 27,515 B JPEG，肉眼见正立。两图 320×180，RGB 均方误差（原样 / 转 180°）为 3980.38 / 14.97。私有截图只放在本机 `/tmp` 供本轮检视，没有入库或公开发送。对象删除后 HEAD 404，恢复日志清。第一次单 PUT 因 SDK 默认 60 秒超时而失败，失败路径也执行删除并 HEAD 404；第二次把一次性探针的 PUT 超时设为 180 秒后通过。为排除超时 PUT 延迟完成，结束后另做前缀库存回读：**0 个对象、0 个 Multipart、0 条对应恢复日志**。服务器临时原片、脚本和 JPEG 已删除并确认目录不存在。服务端未变更正式用户数据或原有云盘对象。
- 对普通无旋转视频另做真实 OSS 回归：2,359 B H.264 MP4 无 Display Matrix；旧参数和 `ar_auto` 的 JPEG 均为 3,573 B，SHA-256 完全相同 `9e348df50e8732c99328b6d5b741c43e34986ec63952b1ad51e25240eb14006d`。精确删除测试对象后 HEAD 404，恢复日志清；临时样本和截图已删除。
- 私有 OSS 视频缩略图按需处理，不保存永久 JPEG；已有视频无需重传。新处理参数属于签名 URL 的一部分，旧 URL 不可自行改参数。iOS 的内存缓存以账号和媒体 ID 为键，已显示的旧图需重新启动 App 才能失效；Android 云盘弹窗和 Desktop 云盘窗口重开时重建临时缩略图目录。

## 部署与旧对象回读

- 部署前，运行中的 `maichat-media.service` 为 active，工作目录 `/opt/maichat/releases/cloud-drive-image-20261025T1700Z-P8k4m/media`，`index.js` SHA-256 `7f213498d0afedb2c1703838cdbcbb2d94594c3d8dba90ecbb527d4e330f5ece`，与修复前本地提交完全一致；其他服务器独有改动未进入该运行文件。
- 建立独立目录 `/opt/maichat/releases/cloud-thumbnail-rotation-20261026T0916Z-77b72c468`，复制原运行包与 `shared` 链接，仅替换 `media/index.js`；主 unit 与 drop-in 备份在新目录的 root-only `backup` 下，所有 EnvironmentFiles 保持原路径。首次切换时版本目录误设为 root 0700，`User=maichat` 无法进入，systemd 报 `200/CHDIR`；部署脚本立即恢复旧 drop-in 并重启旧版，回读 active/旧 cwd/接口 200 与 403。随后将新目录设为 0755，补齐旧包的 `shared` 链接，并用 `sudo -u maichat` 验证逐级遍历、读取 index/server/依赖及模块加载后才重试。
- 第二次切换成功：PID `111444`，active，`/proc/PID/cwd` 与 systemd 工作目录均为新版本；运行 `index.js` SHA-256 `34b160717150541065b6598effc8a84b3d4db4dd7c5920515788de6545e63045`，等于已推送修复文件。环回接口回读：认证回调 200、云盘无内部密钥 403、旧媒体无认证 401、非法头像路径 404；公网回调 200、云盘无认证 403。重启后的服务日志无错误。
- 生产云盘库只读按截图文件名后缀与 18,452,323 B 大小唯一定位旧视频。调用**正在运行的新 signer**为该对象签 `ar_auto` 60 秒私有缩略图 URL，OSS 返回 27,515 B JPEG，SHA-256 `c7d01b6bfe49af442da94644d639f037a3e139c5662d54583e9e6f58724c66e2`，与前述已人工看过的原片测试正立图完全一致。签名 URL、内部密钥和用户原始对象均未输出或修改；只读临时 JPEG 在检查后删除。
- 同轮将 iOS 原 Bundle ID `com.kongshang.maichat` 从 2.2.3 (128) 覆盖安装至 2.2.3 (129)，`devicectl` 回读版本 129、单一 App、数据容器根仍为 Documents/Library/tmp，启动成功且进程 PID 71810。新进程清除了旧的进程内缩略图缓存；真机云盘页面没有通过画面采集再做视觉检查。

## 尚待证明的边界

- 已证明生产旧对象经新服务签发的缩略图像素正立；iOS 真机虽已覆盖安装、重新启动，云盘页的实际显示尚未经画面采集确认。Android 与 Desktop 页面也尚未分别打开验收。若 iOS 页面仍显示旧图，应关闭并重新启动 App 后再看。
