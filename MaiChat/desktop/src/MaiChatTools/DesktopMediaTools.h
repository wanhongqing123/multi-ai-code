// 文件作用：桌面端 Agent 工具；DesktopMediaTools 负责 Agent 工具注册、调用与结果返回，本文件给出其声明。
// 对外边界：本文件给出 DesktopMediaTools 的类型和函数声明；参数、返回值和对象生命周期以此为准。
#pragma once

class MaiToolRegistry;
class QWidget;

void registerDesktopMediaTools(MaiToolRegistry& registry, QWidget* window);
