// 文件作用：提供 OpenSSL 加解密及证书检查工具。
// 对外契约：下方声明公开参数和返回值；实现位于同名源文件。
#pragma once

#include <memory>
#include <string>

#include "MaiError.h"
#include "MaiTool.h"

// 使用 OpenSSL EVP 和受支持的摘要算法计算字节哈希，结果为小写十六进制。
// 不接触密钥、文件或模型状态；算法无效和 OpenSSL 失败返回 MaiError，
// 不跨平台边界抛异常。
MaiResult<std::string> maiOpenSslDigestHex(const std::string& bytes, const std::string& algorithm);

// 只读模型工具：摘要工具接受工作区文件或短文本，证书工具检查一个 PEM/DER X.509 文件。
// 两者均经 MaiToolContext 解析路径并限制读取量。
// 移动端链接固定版本的仓库内 libcrypto，桌面端链接 OpenSSL::Crypto。
std::unique_ptr<MaiTool> makeMaiOpenSslDigestTool();
std::unique_ptr<MaiTool> makeMaiOpenSslCertificateTool();
