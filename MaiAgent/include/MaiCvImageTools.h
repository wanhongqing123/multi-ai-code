// 文件作用：实现 OpenCV 图片处理工具及参数校验。
// 对外契约：下方声明公开参数和返回值；实现位于同名源文件。
#pragma once

#include <memory>
#include <string>

#include "MaiCvImageAnalysis.h"
#include "MaiTool.h"

// 编译了 OpenCV 的宿主提供这个回调；其他宿主不注册图像分析工具。
// 包装层先检查输入和输出路径，再交给分析器。回调在 Agent 工作线程执行，
// 可能解码数千万像素的图片，必须响应取消请求。
using MaiCvImageAnalyzer = MaiCvImageAnalysisResult (*)(const std::string& firstPath,
                                                        const std::string& secondPath,
                                                        const MaiCvImageAnalysisOptions& options,
                                                        const MaiToolContext& context);

std::unique_ptr<MaiTool> makeMaiCvImageQualityTool(MaiCvImageAnalyzer analyzer);
std::unique_ptr<MaiTool> makeMaiCvImageCompareTool(MaiCvImageAnalyzer analyzer);
std::unique_ptr<MaiTool> makeMaiCvFindContoursTool(MaiCvImageAnalyzer analyzer);
std::unique_ptr<MaiTool> makeMaiCvDetectEdgesTool(MaiCvImageAnalyzer analyzer);
std::unique_ptr<MaiTool> makeMaiCvTemplateMatchTool(MaiCvImageAnalyzer analyzer);
std::unique_ptr<MaiTool> makeMaiCvRegisterTranslationTool(MaiCvImageAnalyzer analyzer);
std::unique_ptr<MaiTool> makeMaiCvDetectLinesTool(MaiCvImageAnalyzer analyzer);
std::unique_ptr<MaiTool> makeMaiCvDocumentCornersTool(MaiCvImageAnalyzer analyzer);
std::unique_ptr<MaiTool> makeMaiCvThresholdMaskTool(MaiCvImageAnalyzer analyzer);
