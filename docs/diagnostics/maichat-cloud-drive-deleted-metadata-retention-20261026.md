# 云盘视频删除后元数据永久保留

时间与环境：2026-10-26，macOS 开发机，Node.js v22.19.0 本地测试；源码基线
`8ac971d3104ae447f965e8fb6ea0c0d479e4f248` 加未提交云盘改动。
先在本地复现和修复，随后只对生产灰度账号验证。

## 复现和根因

1. 建立一个云盘视频，完成上传，再由所有者删除。
2. OSS abort/delete 和对象 HEAD 已确认不存在，接口返回 200，配额释放。
3. 查询本地 `cloud_drive_videos`，原实现仍留着 `status='deleted'` 行，
   包含文件名、对象键、大小等账号关联元数据；仅整个账号注销时才删除。

最小回归：`node --test services/auth/cloud-drive-service.test.js` 中的
`private resumable upload, authoritative parts, playback and removal`。先写
“清理后查不到 DB 行”的断言，旧实现失败，实际查到 `{1:1}`。

旧 `sweep()` 为保留 Complete/delete 并发保护，在 OSS 清理成功后只把
`cleanup_pending` 从 1 改为 0，却没有在保护期结束后移除墓碑。直接删除行
又会使尚未返回的 OSS Complete 或 Init 失去清理所需的对象键和 upload ID。

## 修复

- 签名服务增加仅供 loopback 内部密钥调用的 `list-uploads`：账号 ID 与完整对象
  键必须匹配；分页调用 OSS ListMultipartUploads，只返回**精确对象键**的
  upload ID。相邻前缀的其他对象和 ID 不进入响应。
- 账号服务对已删除行先 abort 已知 ID，再按精确键列出并中止其余未完成上传，
  重新列举确认为空；随后 DELETE 对象、HEAD 确认不存在。任何步骤失败均保留
  `cleanup_pending`、元数据和配额供重试。
- `completing` 或 OSS Init 尚未回写 ID 时保留至少 24 小时。Init 迟到的 ID
  回填墓碑并提升 `cleanup_revision`；初始化响应丢失时仍可按精确键找回上传。
  保护期到期后再完成一次 OSS 核验，才用版本号条件删除整行并释放容量。
- 普通已删除视频在同一组 OSS 核验通过且无竞态保护期时删除元数据；账号注销
  仍由现有媒体清理门槛控制。
- 旧版已置 `cleanup_pending=0` 的墓碑也进入独立批次：逐键重复 OSS 核验通过、
  删除时间已超过 24 小时即可直接清除行，不重新扣容量；核验失败或保护期未满
  则恢复待清理状态和容量预留。注销门槛现在要求该账号云盘元数据行数为零。

定向测试覆盖：普通视频删除、OSS 对象仍在、列表失败、abort 后仍列得出上传、
Init 未回写 ID、Init 响应丢失、迟到 Complete、到期清理、旧 sweep 与新墓碑
版本竞争、注销门槛；签名服务用实际 `ali-oss` XML 解析逻辑验证单项/数组与
分页，并验证无效密钥和跨账号对象键被拒绝。云盘定向测试、账号服务全量
`node --test --test-reporter=dot services/auth/*.test.js` 与签名服务全量
`node --test --test-reporter=dot services/oss-media-signer/*.test.js` 均退出 0；
`git diff --check` 退出 0。
旧版墓碑的回归先在原实现得到 3/3 失败，再修复至 4/4 通过；覆盖已清对象、
对象仍在、未满保护期和迟到 Complete。

## 生产灰度回读

2026-10-26，先把签名服务、后把账号服务切到独立版本目录
`/opt/maichat/releases/cloud-drive-cleanup-20261025T1630Z-6Fq9a`；切换前备份了
systemd drop-in 和 SQLite，备份 `quick_check=ok`、外键错误 0。四个部署文件与
本地 SHA-256 一致，进程的工作目录和账号服务 `/health` 已核对。

新签名服务在正式专用 OSS 桶用独立临时对象键走通空列表、一个有效 Multipart ID、
中止后的空列表；无内部密钥返回 403，错账号键返回 400。临时对象、Multipart 和
恢复日志已清零。重启账号服务前，测试账号 A 的六条旧版已删除元数据均为
`cleanup_pending=0`，OSS 对象及 Multipart 为零。重启后六条均转为
`cleanup_pending=1`，进入从删除时间算起的 24 小时保护期；这符合迟到
Init/Complete 的保护逻辑，但尚不能宣称这六条已最终清除。数据库再次
`quick_check=ok`，外键错误 0。公共 HTTPS 回读仍为测试账号 A 200、未放行账号 B
404。随后按用户要求把其当前真机账号单独加入灰度名单，现场读取的服务进程
配置包含 A 与该账号；普通账号仍未全量开放。

尚需在保护期后观察六条元数据被自动清除并确认 OSS 残留继续为零。真实 OSS
分页超过单页的情况由定向测试覆盖，未在生产创建大量分片任务来复测。
