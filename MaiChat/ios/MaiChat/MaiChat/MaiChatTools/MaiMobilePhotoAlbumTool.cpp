// 文件作用：注册 iOS 相册工具，使 Agent 能按权限读取和管理用户授权的照片。
// 把移动端相册读取和写入暴露为 Agent 工具。
// 权限检查与文件路径解析在宿主边界完成，不让模型直接接触系统句柄。
#include "MaiMobileHostTools.h"

void registerPlatformPhotoAlbumTools(MaiToolRegistry& tools,
                                     const std::shared_ptr<MaiMobileHostDispatcher>& dispatcher) {
    addMaiMobileHostTool(tools,
        "mobile_photos_add_to_album",
        "Create or reuse an iOS Photos album and add existing photo IDs to it without duplicating "
        "or removing originals.",
        R"({"type":"object","properties":{"album_name":{"type":"string"},"photo_ids":{"type":"array",)"
            R"("items":{"type":"string"},"minItems":1,"maxItems":50}},"required":["album_name","photo_ids"]})",
        dispatcher, false);
}
