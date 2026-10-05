#pragma once

#include <memory>
#include <string>

#include "MaiCvImageAnalysis.h"
#include "MaiTool.h"

// Hosts that build OpenCV provide this callback; other hosts do not register the image tools.
// The wrapper resolves input paths and output destinations before calling the analyzer. The
// callback runs on an Agent worker, may decode tens of megapixels, and must honor cancellation.
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
