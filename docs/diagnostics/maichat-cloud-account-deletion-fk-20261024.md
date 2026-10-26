# 云盘记录阻断账号注销完成（2026-10-24）

- 场景：账号有私有云盘视频（或视频圈作品）时申请注销，运营完成外部清理后调用 `completeAccountDeletion`。
- 预期：OSS 对象清理完成后，账号数据库记录与用户一并删除；清理未完成时明确拒绝最终删除。
- 根因：`cloud_drive_videos.owner_user_id` 和 `video_circle_posts.author_user_id` 均引用 `users.id`。原 `services/auth/account-deletion.js` 直接删除 `users`，但对应云盘/视频圈元数据仍在，SQLite 外键会拒绝整笔事务。
- 修复：最终注销事务先核验两类媒体记录均已进入删除/撤回状态且 `cleanup_pending=0`；清理未完成时拒绝完成。随后按外键顺序删除视频圈举报、屏蔽、作品和云盘元数据，再删用户。`services/auth/service.test.js` 用同一账号的云盘视频与公开视频证明清理中失败、清理完成后成功且无残留外键。
- 验证：`node --test services/auth/service.test.js` **39/39 PASS**；未部署生产服务，也未操作真实 OSS 对象。
