# MaiChat 私有视频云盘：正式专用 Bucket 的真实 OSS 验收

状态：**真实 OSS 分片、共用 C++ 2 GB 续传及小文件安全验收已通过；三端界面验收待执行**。正式私人云盘专用 Bucket 为
`maichat-private-cloud-drive-gz-20261025-f346b709`（`oss-cn-guangzhou`，
Standard/LRS）。它与现有 `maichat-ai-video-resource` 素材桶分离。以下步骤须在
云盘生产开关关闭、测试账号和独立账号数据库下进行；不要在素材桶上执行。
2026-10-25 在新桶随机测试账号前缀中实测：错误长度分片被 OSS 以
`403 SignatureDoesNotMatch` 拒绝，错误 MD5 被 `400 InvalidDigest` 拒绝；
正确 8 MiB 分片 PUT 返回 200，签名服务 ListParts 回读第 1 片为
8,388,608 字节。随后 Abort，OSS 对象与未完成分片回读均为 0。
测试未启用生产账号服务或生产云盘开关。单分片 ListParts 的真实 SDK
返回形状曾触发签名服务 502，修复证据见
[`docs/diagnostics/maichat-cloud-listparts-single-part-20261025.md`](../diagnostics/maichat-cloud-listparts-single-part-20261025.md)。
同日还用临时 SQLite 账号服务、loopback 签名器和共用 C++ 客户端在同地域 ECS
内网地址上传恰好 2,000,000,000 字节：首片后暂停、新进程复用同一会话续传、
OSS HEAD 长度核对、60 秒签名 GET 全量 SHA-256 与源文件一致；另一测试账号
列表为空且无法取得播放链接，删除后对象和未完成 Multipart 均为 0。
所用源文件为稀疏全零传输样本，**不证明视频可解码播放**；三端真实 UI 与
生产 Nginx 云盘路由仍待验收。专用 RAM 身份已创建，访问新桶成功、访问旧素材桶
返回 403，并用该身份再次完成真实 8 MiB 上传、合并、私有读取和删除闭环。
ECS 经公网签名地址的 2 GB 测试
吞吐过低，主动停止后残留清点为 0；公网 8 MiB 单片及完整小文件闭环已通过。
2026-10-25 用正式限权 RAM 在随机临时账号前缀中实测：同键覆盖被 OSS 拒绝，
原对象 SHA-256 不变；60 秒签名 GET 立即读取 200、75 秒后同一 URL 返回 403；
临时 SQLite 账号注销时，已完成视频和未完成分片均清理，OSS 残留与恢复日志为 0。
生产 auth/media 已切到新代码但保持云盘开关关闭；数据库 quick_check=ok、外键 0，
原 55 个用户和 77 条会话保留。
2026-10-26 新增的精确键 `list-uploads` 与删除后元数据清理目前只有本地
SDK/数据库并发测试，**尚未在真实 OSS 或运行中的服务上验收**。

## 1. 隔离环境和停止条件

使用上述正式私人云盘专用 Bucket、云盘专用 RAM 身份、独立账号服务数据库、
独立签名服务，以及两个互不关联的测试账号 A/B。三端测试包只指向这套账号服务。
只在这两个测试账号的 `cloud-drive/<账号>/` 前缀下写入、读取和清理；绝不能使用
`maichat-ai-video-resource`、生产数据库或真实用户账号。运行期间不要把
`MAICHAT_CLOUD_DRIVE_ENABLED=1` 加到生产配置。

在仓库外创建权限为 `0600` 的环境文件、ossutil 配置和 A/B Bearer 文件。不要在命令行参数、终端历史、HTTP 调试日志、CI 产物或验收记录中留下 AccessKey、Bearer、`upload_url`、`read_url`、内部密钥。关掉 shell `xtrace`，不要用 `curl -v` 或保存完整 HTTP 请求。只记录 HTTP 状态、OSS 错误码、对象大小、摘要及脱敏的对象 ID。

先完成以下只读检查，再启动本地服务。示例使用 ossutil 2 的 API 子命令，`$ACCEPTANCE_OSSUTIL_CONFIG` 是仓库外的专用配置文件。操作员须核对返回值：Bucket ACL `private`，版本控制**从未启用**（OSS 查询为空/Null），有只针对 `cloud-drive/` 的未完成 Multipart 自动 Abort 规则。`Suspended` 表示曾启用过，不能当成从未启用；发现 `Enabled`、`Suspended`、公开读、未知状态或无生命周期规则时停止。

```sh
test "$MAICHAT_CLOUD_DRIVE_OSS_BUCKET" = 'maichat-private-cloud-drive-gz-20261025-f346b709' || exit 1
test "$MAICHAT_CLOUD_DRIVE_OSS_BUCKET" != "$MAICHAT_OSS_BUCKET" || exit 1
ossutil api get-bucket-acl --bucket "$MAICHAT_CLOUD_DRIVE_OSS_BUCKET" --config-file "$ACCEPTANCE_OSSUTIL_CONFIG" --output-format json
ossutil api get-bucket-versioning --bucket "$MAICHAT_CLOUD_DRIVE_OSS_BUCKET" --config-file "$ACCEPTANCE_OSSUTIL_CONFIG" --output-format json
ossutil api get-bucket-lifecycle --bucket "$MAICHAT_CLOUD_DRIVE_OSS_BUCKET" --config-file "$ACCEPTANCE_OSSUTIL_CONFIG" --output-format json
```

服务端配置须有同一个 32 字节以上的 `MAICHAT_CLOUD_DRIVE_INTERNAL_KEY`、`MAICHAT_CLOUD_DRIVE_ENABLED=1`、测试 Bucket/地域及专用 RAM AK；账号服务只能 loopback 调签名服务。确认 RAM 仅可操作该 Bucket 的 `cloud-drive/*`，具备 Multipart 初始化、分片、列举、完成、终止、HEAD/GET、删除及清点未完成上传所需权限。先在**隔离测试环境**执行现有本地测试：

```sh
(cd services/auth && npm test)
(cd services/oss-media-signer && npm test)
```

部署新代码前还需在临时账号前缀验证内部 `list-uploads`：用相同精确对象键
建立两个未完成 Multipart，再建立一个只共享此前缀的相邻对象键；A 的内部
密钥请求须只返回前两个 ID，分页跨页时也不能漏掉或返回邻键。错误内部密钥、
B 的账号 ID 与 A 的对象键组合必须被拒绝。删除 A 的视频后，OSS
ListMultipartUploads 中这两个 ID 与对象 HEAD 都须不存在，元数据只在保护期
结束并复查后删除；任意列举或 abort 失败须保留 `cleanup_pending` 和预留容量。
此项是新清理逻辑的独立真实 OSS 门槛，不能用此前单分片 ListParts 结果代替。
部署后还须用脱敏只读记录核对旧版 `status='deleted', cleanup_pending=0` 的
canary 行：后台应逐键复查；已清理干净且删除超过 24 小时的行消失而账号容量不
重新增加，无法证明清理完成的行恢复待清理与容量预留，账号注销在任一行仍在时
保持 pending。不要直接对生产数据库执行测试性 UPDATE/DELETE。

若任一项失败，停止真实 OSS 验收。服务端路由、大小与配额以 [账号服务接口](../../services/auth/README.md) 和 [签名服务接口](../../services/oss-media-signer/README.md) 为准。

## 2. 小文件签名与访问控制

用 A 在测试账号服务创建 **8 MiB** `probe.mp4`：`POST /v1/cloud-drive/videos/uploads`，JSON 为 `{"name":"probe.mp4","size_bytes":8388608,"content_type":"video/mp4"}`。要求 HTTP 201，记录 `video_id` 和 `upload_id` 到只读验收记录，不记录签名 URL。构造 8 MiB 随机文件，计算原始 MD5 的 Base64；调用 `POST /v1/cloud-drive/videos/uploads/{video_id}/parts/1/sign`，JSON 为 `{"content_md5_base64":"<BASE64>"}`。响应须为 200，`part_size_bytes=8388608`，`upload_headers.Content-Length=8388608`，`Content-MD5` 与提交值相同，`expires_in=300`。以下示例只连接预先隔离的本地账号服务；`$A_TOKEN_FILE` 是仓库外 `0600` 文件，`$PROBE_DIR` 是临时目录。响应只留在 shell 变量，不打印签名 URL。

```sh
test "$AUTH_BASE" = 'http://127.0.0.1:9100' || exit 1
test -r "$A_TOKEN_FILE" && test -d "$PROBE_DIR" || exit 1
maichat_auth() {
  token_file=$1; shift
  printf 'header = "Authorization: Bearer %s"\n' "$(tr -d '\r\n' < "$token_file")" |
    curl --silent --show-error --config - "$@"
}
dd if=/dev/urandom of="$PROBE_DIR/probe.mp4" bs=1048576 count=8 status=none
probe_md5=$(openssl dgst -md5 -binary "$PROBE_DIR/probe.mp4" | openssl base64 -A)
created=$(maichat_auth "$A_TOKEN_FILE" -X POST -H 'Content-Type: application/json' \
  --data '{"name":"probe.mp4","size_bytes":8388608,"content_type":"video/mp4"}' \
  "$AUTH_BASE/v1/cloud-drive/videos/uploads")
video_id=$(printf '%s' "$created" | jq -r '.video_id')
test "$video_id" != null && test -n "$video_id" || exit 1
sign_body=$(jq -nc --arg md5 "$probe_md5" '{content_md5_base64:$md5}')
signed=$(maichat_auth "$A_TOKEN_FILE" -X POST -H 'Content-Type: application/json' \
  --data "$sign_body" \
  "$AUTH_BASE/v1/cloud-drive/videos/uploads/$video_id/parts/1/sign")
upload_url=$(printf '%s' "$signed" | jq -r '.upload_url')
test "$upload_url" != null && test -n "$upload_url" || exit 1
```

每次 PUT 后调用 A 的 `GET /v1/cloud-drive/videos/uploads/{video_id}/parts` 核对 OSS 实际分片。不要把 URL 直接放进 `curl` 命令行或输出；用 `curl --config -` 从标准输入传 `url = "..."`，并以 `--upload-file` 发送文件，避免 `--data-binary` 自动生成额外 Content-Type。先做下表的两项负例，再运行正确 PUT 的最小命令；HTTP 2xx 仍要以 ListParts 核对。重放正确 PUT 可测同一片号的幂等边界。

```sh
put_code=$(printf 'url = "%s"\n' "$upload_url" |
  curl --silent --show-error --config - --upload-file "$PROBE_DIR/probe.mp4" \
    -H 'Content-Length: 8388608' -H "Content-MD5: $probe_md5" \
    -o /dev/null -w '%{http_code}')
printf 'OSS PUT HTTP %s\n' "$put_code"
maichat_auth "$A_TOKEN_FILE" \
  "$AUTH_BASE/v1/cloud-drive/videos/uploads/$video_id/parts" |
  jq '{status,parts:[.parts[] | {part_number,size_bytes,etag}]}'
```

| 次序 | PUT 内容及请求头 | 必须看到 |
|---|---|---|
| 1 | 用**内部测试签名服务**对同一测试对象、upload ID、片号 1 签 `expected_bytes=8388608`，但 `content_md5_base64` 用 1 MiB 文件的真实 MD5；随后 PUT 该 1 MiB 文件，`Content-MD5` 与内容匹配，实际 `Content-Length: 1048576`。签名 URL 应显示 `x-oss-additional-headers=content-length`。 | OSS 非 2xx；ListParts 无该片。这样只有长度与签名不同。若成功，长度签名约束无效，停止上线。 |
| 2 | 用上述账号 API 的正确 8 MiB 签名 URL，PUT 长度不变但篡改 1 字节，仍带原签名 MD5 请求头。 | OSS 非 2xx，通常为 `BadDigest`；ListParts 无该片。 |
| 3 | 用账号 API 的 URL，PUT 8 MiB 正确内容及签名返回的两个请求头。 | OSS 2xx；ListParts 仅有第 1 片，大小 8388608，ETag 有效。 |
| 4 | 再用第 3 项 URL 和原文件重放同一片。 | 即使 OSS 返回 2xx，也只能留下同一个片号、相同大小/ETag；不能增加容量或对象数。 |

用 B 的 Bearer 请求 A 的 `GET parts`、`POST sign`、`POST complete`、`GET play-link`、`DELETE`，都应为 404；B 的列表不得出现 A 的视频。用 A `POST /complete`，要求 200 且再次提交仍为 200；A 列表仅出现一条视频，配额已用量 8388608。直接访问未签名对象 URL 应拒绝；A 的 `GET /play-link` 返回 60 秒私有 URL，立即读取应成功，等待至少 75 秒重用同一 URL 应失败。签名 URL 本身是短期访问凭据，不能用已泄露的 URL 验证账号隔离。

对同一对象键验证防覆盖：从**隔离数据库**只读取得该 `video_id` 的 `object_key`，用内部签名服务对同一键尝试再次 Multipart init。若 init 因对象已存在而失败，记录 OSS 错误码；若 init 成功，上传一片后用同一服务尝试 complete，必须因 `x-oss-forbid-overwrite: true` 失败，并显式 abort 第二个 upload ID。随后 HEAD 和读取原对象，确认大小与 SHA-256 未变。**不能**为此在生产对象上试验。检查对象 ACL 返回 private：

```sh
ossutil api get-object-acl --bucket "$MAICHAT_CLOUD_DRIVE_OSS_BUCKET" --key "$object_key" \
  --config-file "$ACCEPTANCE_OSSUTIL_CONFIG" --output-format json
```

## 3. 中断、删除与生命周期

新建第二个 16 MiB 视频并成功 PUT 第 1 片；A 的 `GET parts` 应只显示 1/2。调用 `DELETE /v1/cloud-drive/videos/uploads/{id}`，要求 200 或明确的 202 待清理；视频立即不在本人列表中，B 一直不可见。用隔离 Bucket 清点该账号的未完成上传，核对该 upload ID 最终消失，并核对 HEAD 不存在、配额在清理完成后释放：

```sh
test -n "$A_ACCOUNT_ID" || exit 1
ossutil api list-multipart-uploads --bucket "$MAICHAT_CLOUD_DRIVE_OSS_BUCKET" \
  --prefix "cloud-drive/$A_ACCOUNT_ID/" \
  --config-file "$ACCEPTANCE_OSSUTIL_CONFIG" --output-format json
```

删除已完成视频同样检查列表、私有 GET 失效、对象不存在；如返回 202，须等待重试清理结束，不能把 202 当作已删除 OSS 数据。

故意用内部签名服务 init 一个测试账号前缀的孤儿 Multipart，不写账号数据库。
确认 `list-multipart-uploads` 可见。正式 Bucket 的 `cloud-drive/` 未完成分片规则
应为 8 天，不要为了加速测试而缩短整桶规则；通过 OSS 管理 API 显式 abort 该孤儿，
确认 upload ID 消失。生命周期的实际定时触发需等待 8 天后单独验收并记录，不可用
规则已配置来冒充定时清理已验证。账号注销另用隔离账号跑一次：已有视频、进行中上传
同时存在时发起注销，账号数据只有 OSS 清理完成后才能真正清除；检查无残留对象或 Multipart。

### 3.1 正式云盘 Bucket 的剩余安全验收（准备完成，尚未执行）

`services/auth/scripts/cloud-drive-security-acceptance.js` 将在 ECS 上读取
`/etc/maichat/cloud-drive.env` 的专用 RAM 凭据，使用**临时 SQLite 和 loopback
账号／签名服务**，生成一个 `test_<随机 16 位十六进制>` 账号前缀。脚本会在该前缀
创建一条 8 MiB 已完成对象、一条只上传首片的 16 MiB 待续传对象，并验证：

1. 已存在的对象键在 `x-oss-forbid-overwrite: true` 下不能被第二次 Multipart
   初始化或完成所覆盖；原对象 SHA-256 保持不变。
2. 同一 60 秒签名 GET 立即读取成功，75 秒后再用同一 URL 读取返回 403。
3. 临时账号提交注销后，已完成对象和待续传 Multipart 都清理为零，临时 SQLite
   元数据可完成注销。脚本不接触生产数据库、生产用户和素材桶。

执行前须确认正式桶的 private ACL、未启用版本控制及 8 天未完成分片清理规则。
只有明确执行参数才允许写入，且生产开关保持关闭：

```sh
node services/auth/scripts/cloud-drive-security-acceptance.js \
  --env-file=/etc/maichat/cloud-drive.env \
  --execute=maichat-private-cloud-drive-gz-20261025-f346b709
```

脚本在 root-only `/var/lib/maichat/oss-smoke-recovery/` 中**先写恢复记录再写 OSS**，
结束时清点并清理整个随机测试账号前缀。失败且清理未证明时保留恢复记录；只能用
脚本提供的 `--recover=security-<UUID>.json` 对同一随机前缀重试清理，不能删除
其他账号。如果进程在 Multipart Complete 已发出但结果未知时退出，恢复程序会先清理
随机前缀，但保留记录至少 24 小时；到期后须再次执行同一 `--recover` 并清点，
才能删除恢复记录。执行约需至少 75 秒；日志仅记录 HTTP 状态、OSS 错误码与哈希比较结果，
不记录凭据、Bearer、对象键或签名 URL。上述三项在实际运行前仍标记为**未验收**。

## 4. 2 GB 边界与三端续传

准备一个恰好 **2,000,000,000 字节**的 `.mp4` 传输测试文件，记录源文件 SHA-256；它只用于字节/断点验证，实际播放另用小的有效 MP4。先在 Desktop 测试包通过正常界面上传，至少完成两片后暂停并退出进程；重启后用同一账号继续，要求复用原 `video_id/upload_id`，先读 OSS ListParts，仅上传缺失片。最终应为 239 片，最后一片 3,511,296 字节；Complete 200，HEAD `Content-Length=2000000000`，下载文件 SHA-256 等于源文件。跨设备登录同一账号，iOS、Android 列表均看到视频并可下载；各自还需以本地小视频走一次真实 Bucket 上传、暂停、杀进程、恢复、播放和删除。另一账号列表始终为空。

边界请求再分别提交 2,000,000,001 字节文件（400）及超过账号剩余 10,000,000,000 字节的并发预留（413），确认没有启动 OSS Multipart。用两个同账号客户端同时开始接近剩余额度的上传，只允许一个原子预留成功。删除、取消、注销与 7 天过期时，待清理配额不得提前释放。

## 5. 记录与放行

保留一张验收表，每行记录：测试日期、客户端/服务端提交与运行路径、Bucket 的**脱敏别名**、账号 A/B 的脱敏 ID、操作、预期与实测 HTTP/OSS 状态、对象长度/摘要、清理完成时间。不要保存完整对象键、签名 URL 或令牌。所有测试对象及未完成 Multipart 最终清点为零。任何签名长度、MD5、隔离、覆盖、清理或 2 GB 续传失败，都保持云盘生产开关关闭，并把真实 OSS 错误码和复现步骤写入 `docs/diagnostics/` 后修复重测。

OSS 官方参考：[ossutil 2 命令与 `--config-file` 参数](https://www.alibabacloud.com/help/en/oss/developer-reference/ossutil-overview/)、[Bucket ACL](https://www.alibabacloud.com/help/en/oss/developer-reference/get-bucket-acl)、[版本控制](https://www.alibabacloud.com/help/en/oss/developer-reference/get-bucket-versioning)、[生命周期](https://www.alibabacloud.com/help/en/oss/developer-reference/get-bucket-lifecycle)、[列举未完成 Multipart](https://www.alibabacloud.com/help/en/oss/developer-reference/list-multipart-uploads)、[对象 ACL](https://www.alibabacloud.com/help/en/oss/developer-reference/get-object-acl)、[V4 签名与附加请求头](https://www.alibabacloud.com/help/en/oss/developer-reference/add-signatures-to-urls)、[预签名 URL 请求头一致性](https://www.alibabacloud.com/help/en/oss/user-guide/upload-files-using-presigned-urls)、[Multipart 与防覆盖](https://www.alibabacloud.com/help/en/oss/user-guide/multipart-upload)、[未完成分片清理](https://www.alibabacloud.com/help/en/oss/user-guide/delete-parts)。
