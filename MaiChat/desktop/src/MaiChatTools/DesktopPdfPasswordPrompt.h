// 文件作用：为新生成的 PDF 从本地安全表单取得打开密码，不经过工具参数或结果。
// 仅工作线程可调用；取消只关闭本次表单。返回值只供普通加密 API 短期使用，不应序列化。
#pragma once

#include <atomic>
#include <string>

#include "MaiError.h"

MaiResult<std::string> requestDesktopPdfPassword(const std::atomic<bool>* cancel);
