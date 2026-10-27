// 文件作用：声明把 Agent 工具调用转发给移动端宿主，并管理处理器注册、并发调用与释放所需的接口和数据结构。
// 调用边界：平台实现与上层调用方通过本文件约定参数、所有权和释放方式。
#pragma once

#include <memory>
#include <string>

#include "MaiMobileAgent.h"
#include "MaiTool.h"

class MaiMobileHostDispatcher;

std::shared_ptr<MaiMobileHostDispatcher> makeMaiMobileHostDispatcher();
bool setMaiMobileHostToolHandler(const std::shared_ptr<MaiMobileHostDispatcher>& dispatcher,
                                 void* context, MaiMobileHostToolHandler handler,
                                 MaiMobileHostToolResponseFree responseFree,
                                 MaiMobileHostToolContextRelease contextRelease);
void clearMaiMobileHostToolHandler(const std::shared_ptr<MaiMobileHostDispatcher>& dispatcher);
MaiToolResult callMaiMobileHostTool(const std::shared_ptr<MaiMobileHostDispatcher>& dispatcher,
                                   const std::string& name, const std::string& argumentsJson);

void addMaiMobileHostTool(MaiToolRegistry& tools, const char* name, const char* description,
                          const char* schema,
                          const std::shared_ptr<MaiMobileHostDispatcher>& dispatcher,
                          bool approval);
void registerMaiChatHostTools(MaiToolRegistry& tools,
                              const std::shared_ptr<MaiMobileHostDispatcher>& dispatcher);
void registerMobileScreenCaptureTools(MaiToolRegistry& tools,
                                      const std::shared_ptr<MaiMobileHostDispatcher>& dispatcher);
void registerMobileBrowserTools(MaiToolRegistry& tools,
                                const std::shared_ptr<MaiMobileHostDispatcher>& dispatcher);
void registerMobilePhotoTools(MaiToolRegistry& tools,
                              const std::shared_ptr<MaiMobileHostDispatcher>& dispatcher);
void registerPlatformPhotoAlbumTools(MaiToolRegistry& tools,
                                     const std::shared_ptr<MaiMobileHostDispatcher>& dispatcher);
