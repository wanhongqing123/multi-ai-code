#pragma once

#include <memory>

#include "MaiTool.h"

// webfetch：抓一个网页转成纯文本。
//
// **唯一一个往外发数据的工具**，所以每个新域名都要用户点一次头：
// 模型读到的内容可能被提示词注入污染，而被诱导的模型能把内容编进 URL 发出去。
std::unique_ptr<MaiTool> makeMaiWebFetchTool();
