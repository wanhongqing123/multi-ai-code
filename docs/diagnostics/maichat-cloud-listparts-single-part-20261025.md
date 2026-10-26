# 云盘真实 OSS 首片上传后 ListParts 返回 502

- 时间：2026-10-25；服务：Node.js 素材签名服务 `services/oss-media-signer`；OSS SDK：`ali-oss` 6.23.0。
- 源码基线：`8ac971d3104ae447f965e8fb6ea0c0d479e4f248` 加本地未提交的云盘实现。真实 OSS 验收服务运行产物路径和版本尚未记录，不能把本地测试当成已部署修复。
- 现场步骤：在正式云盘专用 Bucket 初始化 Multipart，上传正确的首个 8 MiB 分片，OSS PUT 返回 200，随后调用签名服务 `/cloud-drive/list-parts`。验收脚本的 `ListParts1` 判定失败。
- 最小复现：让安装的 `ali-oss` SDK `listParts()` 接收单个 `<Part>` 的解析结果，即 `result.data.Part` 为对象；服务端 `for (const part of page.parts || [])` 对该对象迭代。修复前定向测试得到 HTTP 502，期望 200。
- 根因证据：SDK `node_modules/ali-oss/lib/common/multipart.js` 直接返回 `parts: result.data.Part || []`。XML 解析器在仅一个 `<Part>` 时给对象，多项时给数组；旧服务端只接受数组，触发 `TypeError: object is not iterable` 并落入通用 502 捕获。OSS PUT 200 不代表服务端能解析 ListParts。
- 修复：`services/oss-media-signer/index.js` 在遍历前将单个 Part 对象规范化为单元素数组，同时保留数组和空结果处理。`cloud-drive.test.js` 用真实 SDK 方法与模拟单节点 XML 响应覆盖这一返回形态。
- 本地验证：定向回归修前 FAIL（502），修后 PASS（200，返回单个 8 MiB 分片及 ETag）；签名服务全量 `npm test` 45/45 PASS，`git diff --check` PASS。
- 未验证边界：本修复尚未部署到远端签名服务，也尚未重新跑真实 OSS 首片上传与 ListParts。真实服务返回的原始 OSS XML 和请求 ID 未保存在本记录中；部署后需确认首片与多片、分页及完成操作。
