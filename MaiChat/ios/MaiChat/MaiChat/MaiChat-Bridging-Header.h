// 文件作用：向 Swift 暴露 MaiAgent、ONNX 和本地媒体工具所需的 C/C++ 桥接声明。
// 向 Swift 模块公开受控的 C/Objective-C 桥接声明。
// 原生实现和资源所有权仍由对应 C++／Objective-C 文件管理。
#include "../../../MobileAgentAdapter/MaiMobileAgent.h"
#include <onnxruntime_c_api.h>
#include "../../../MaiChatTools/MaiImageFilter.h"
#include "../../../Media/MaiGraphicsPresenter.h"
#include "../../../Media/MaiImageDecode.h"
#include "../../../Media/FfplayCompat/MaiFfplayEntry.h"
#include "Media/MaiFfplayIosBridge.h"
