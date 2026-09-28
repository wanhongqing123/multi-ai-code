#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>

#include "MaiTool.h"

// PDF 排版由宿主提供：桌面用 Qt，移动端用各自系统的 HTML 打印能力。
//
// 核心工具负责解析参数、限定工作区路径、审批和验证 PDF 产物。renderer 只接收
// 已读取的 HTML 与已校验的绝对输出路径，不能自行从模型参数中拼路径。
// 回调在 Agent 工作线程同步执行；宿主若要使用 UI 排版对象，须自行派发到主线程
// 并在返回前完成写入。取消标志只在回调期间有效，空指针表示调用方没有取消源。
using MaiPdfRenderer =
    std::function<MaiToolResult(const std::string& htmlUtf8, const std::string& absoluteOutputPath,
                                const std::atomic<bool>* cancel)>;

// renderer 为空时不注册工具。生成动作会写文件，因此遵循普通写工具的审批策略。
std::unique_ptr<MaiTool> makeMaiPdfTool(MaiPdfRenderer renderer);
