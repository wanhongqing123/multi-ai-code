# 云盘完成上传与删除并发时的对象残留

时间与环境：2026-10-25，macOS 开发机，Node.js v22.19.0，源码基线
`8ac971d3104ae447f965e8fb6ea0c0d479e4f248` 加未提交云盘改动。
云盘默认关闭；未接触生产 OSS 或生产服务。

## 复现

1. 用本地假 OSS 建立一个已上传全部分片的云盘视频。
2. 让 `completeUpload()` 在发往 OSS 的异步调用中等待。
3. 同时删除该视频。修复前删除返回 200 且 `cleanup_pending=0`。
4. 再放行 OSS 完成请求，使对象在此前删除操作之后出现。

最小失败用例：
`node --test --test-name-pattern='delete during OSS completion' services/auth/cloud-drive-service.test.js`。
修复前结果为 `200 !== 202`：删除操作误认为对象清理已经完成。
同一时序也可由上传会话到期清理、账号注销触发。

## 根因和影响

`cloud-drive-service.js` 的 `complete()` 在外部 OSS 完成调用期间释放了控制权。
`remove()` / `sweep()` / `cleanupAccount()` 可先把记录设为 `deleted`，执行当时
尚无对象的 DELETE，并清除 `cleanup_pending`。外部 Complete 随后创建对象，
`complete()` 的 `ready` 条件更新失败；旧代码再次调用 `sweep()` 时因没有待清理行，
不会删除迟到的私有对象。若此时账号注销完成，元数据甚至可先被移除。

签名服务原先把 HEAD 成功但大小或类型不符返回成 `exists:false`，也会使清理
错误地相信异常对象不存在。

## 修复与验证

删除 `completing` 视频时保留至少 24 小时的持久清理租约和配额占用。
每次清理执行 OSS abort、DELETE，再用 HEAD 确认对象不存在；租约到期前
按小时重试。`cleanup_revision` 防止较早的异步清理覆盖迟到 Complete 重新排队
的状态。Complete 返回后若发现视频已删除，立即重新排队并再清理。
签名服务 HEAD 成功始终返回 `exists:true`，并单独提供实际大小和类型。
账号最终注销已有 `cleanup_pending` 检查，因此在租约到期并证实清理前无法完成。
2026-10-26 的后续修复在完成精确对象键 Multipart 列举、abort、对象 HEAD
复核及该租约后，会删除整个墓碑元数据行；详见
[`maichat-cloud-drive-deleted-metadata-retention-20261026.md`](maichat-cloud-drive-deleted-metadata-retention-20261026.md)。

本地 deferred Promise 回归覆盖：删除后迟到完成、到期后迟到完成、注销期间
迟到完成、旧清理请求与新墓碑竞态，以及最终注销门槛。定向云盘测试 8/8 通过；
真实账号注销路由测试也证实完成中的视频保留清理租约。生产 OSS、长达 24 小时的
真实异步请求和崩溃恢复未验收。真实 OSS Bucket 的版本、禁覆盖与签名长度约束
仍须在开启云盘前单独验收。
