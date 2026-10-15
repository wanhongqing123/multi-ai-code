// 文件作用：实现 OpenCV 视频处理工具及输出管理。
// 对外契约：下方声明公开参数和返回值；实现位于同名源文件。
#pragma once

#include <memory>
#include <string>

#include "MaiCvVideoAnalysis.h"
#include "MaiTool.h"

// The host supplies one in-process OpenCV/FFmpeg video analyzer. Factories
// return null when it is unavailable, so unsupported builds never advertise
// tools they cannot execute. The callback runs on the Agent tool worker and
// must observe MaiToolContext cancellation during long decodes. Its input path
// is already resolved and verified by the tool; it must not modify that file.
using MaiCvVideoAnalyzer = MaiCvVideoAnalysisResult (*)(const std::string& path,
                                                        const MaiCvVideoAnalysisOptions& options,
                                                        const MaiToolContext& context);

std::unique_ptr<MaiTool> makeMaiCvSceneDetectTool(MaiCvVideoAnalyzer analyzer);
std::unique_ptr<MaiTool> makeMaiCvMotionDetectTool(MaiCvVideoAnalyzer analyzer);
