// 文件作用：桌面端 Agent 工具；MaiChatHostTools 负责 Agent 工具注册、调用与结果返回，本文件给出其声明。
// 对外边界：本文件给出 MaiChatHostTools 的类型和函数声明；参数、返回值和对象生命周期以此为准。
#pragma once

class MaiToolRegistry;
class RemoteIMApplication;

// Register tools that are owned by the MaiChat host. MaiAgent remains unaware
// of Qt, contacts, conversations, and message delivery; another application
// embedding MaiAgent can register a completely different tool set through the
// same registry extension point.
void registerMaiChatHostTools(MaiToolRegistry &registry,
                              RemoteIMApplication &app);
