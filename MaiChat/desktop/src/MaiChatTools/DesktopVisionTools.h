// 文件作用：桌面端 Agent 工具；DesktopVisionTools 负责 Agent 工具注册、调用与结果返回，本文件给出其声明。
// 对外边界：本文件给出 DesktopVisionTools 的类型和函数声明；参数、返回值和对象生命周期以此为准。
#pragma once

class MaiToolRegistry;

// 注册复用 OpenCV C API 的桌面图片分析工具。
// 每个 Agent 实例只从 Qt 资源加载一次模型。
void registerDesktopVisionTools(MaiToolRegistry& registry);
