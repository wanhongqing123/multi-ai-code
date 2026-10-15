// 文件作用：实现 OpenCV 视频处理工具及输出管理。
// 对外契约：下方声明公开参数和返回值；实现位于同名源文件。
#pragma once

#include <memory>
#include <string>

#include "MaiCvVideoAnalysis.h"
#include "MaiTool.h"

// 宿主提供一个进程内 OpenCV/FFmpeg 视频分析器；不可用时工厂返回空，
// 使不支持的构建不会向模型宣称有无法执行的工具。
// 回调在 Agent 工具线程运行，长时间解码期间须检查 MaiToolContext 的取消状态。
// 输入路径已由工具解析并验证，分析器不得修改源文件。
using MaiCvVideoAnalyzer = MaiCvVideoAnalysisResult (*)(const std::string& path,
                                                        const MaiCvVideoAnalysisOptions& options,
                                                        const MaiToolContext& context);

std::unique_ptr<MaiTool> makeMaiCvSceneDetectTool(MaiCvVideoAnalyzer analyzer);
std::unique_ptr<MaiTool> makeMaiCvMotionDetectTool(MaiCvVideoAnalyzer analyzer);
