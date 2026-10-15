// 文件作用：获取网页内容并转换为模型可读文本。
// 对外契约：下方声明公开参数和返回值；实现位于同名源文件。
#pragma once

#include <memory>
#include <string>

#include "MaiTool.h"

// curl_fetch：抓一个网页转成纯文本。
//
// 会向外部服务发请求，所以每个新域名都要用户点一次头：
// 模型读到的内容可能被提示词注入污染，而被诱导的模型能把内容编进 URL 发出去。
// 可选的根证书 PEM 路径与模型客户端一致；空时使用平台默认值。
std::unique_ptr<MaiTool> makeMaiWebFetchTool(std::string caBundlePath = {});

// Convert already-fetched HTML to the same readable text as curl_fetch, without network I/O.
std::string maiHtmlToText(std::string html);
