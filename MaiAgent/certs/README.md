# iOS HTTPS CA 根证书

`cacert.pem` 来自 curl 官方提供的 Mozilla CA 根证书提取文件：
<https://curl.se/ca/cacert.pem>。当前快照标记为 2026-09-25，SHA-256：
`a41b5d356aea97a529fe27e0f7316d2f9d946d75927476cf9cf1b90637d00505`。

iOS 的共享 C++ libcurl 使用 OpenSSL 后端，App 将此文件打包为只读资源并
通过 `caBundle` 传给 MaiAgent。更换文件时从官方地址下载，核对 HTTPS 和
文件哈希后重新构建 App。证书数据源遵循 Mozilla Public License 2.0；
文件头部保留来源和日期信息。
