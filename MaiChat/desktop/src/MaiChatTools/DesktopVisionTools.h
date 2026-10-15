// 文件作用：桌面端 Agent 工具；DesktopVisionTools 负责 Agent 工具注册、调用与结果返回，本文件给出其声明。
// 对外边界：本文件给出 DesktopVisionTools 的类型和函数声明；参数、返回值和对象生命周期以此为准。
#pragma once

class MaiToolRegistry;

// Register desktop image-analysis tools backed by the shared OpenCV C API.
// Models are loaded from Qt resources once per Agent instance.
void registerDesktopVisionTools(MaiToolRegistry& registry);
