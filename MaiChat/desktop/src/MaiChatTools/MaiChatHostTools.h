// 文件作用：桌面端 Agent 工具；MaiChatHostTools 负责 Agent 工具注册、调用与结果返回，本文件给出其声明。
// 对外边界：本文件给出 MaiChatHostTools 的类型和函数声明；参数、返回值和对象生命周期以此为准。
#pragma once

class MaiToolRegistry;
class RemoteIMApplication;

// 注册由 MaiChat 宿主持有的工具。MaiAgent 不认识 Qt、联系人、会话或消息交付；
// 其他嵌入 MaiAgent 的应用可通过同一注册扩展点提供完全不同的工具组。
void registerMaiChatHostTools(MaiToolRegistry &registry,
                              RemoteIMApplication &app);
