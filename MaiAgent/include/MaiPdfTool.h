// 文件作用：读取和处理 PDF 文档供 Agent 使用。
// 对外契约：下方声明公开参数和返回值；实现位于同名源文件。
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>

#include "MaiTool.h"
#include "MaiPdfDocumentTools.h"

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
// passwordProvider 仅处理 password_protected=true 的私有输入，不把密码放入模型参数或结果。
// 未提供时普通 PDF 不受影响；请求密码保护则明确返回 NotConfigured，绝不退回明文。
std::unique_ptr<MaiTool> makeMaiPdfTool(MaiPdfRenderer renderer,
                                        MaiPdfPasswordProvider passwordProvider = {});
