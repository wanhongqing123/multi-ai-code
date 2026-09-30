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
void registerMobilePhotoTools(MaiToolRegistry& tools,
                              const std::shared_ptr<MaiMobileHostDispatcher>& dispatcher);
void registerPlatformPhotoAlbumTools(MaiToolRegistry& tools,
                                     const std::shared_ptr<MaiMobileHostDispatcher>& dispatcher);
