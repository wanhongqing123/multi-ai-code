// 文件作用：Android 移动端 Agent 工具与宿主能力；MaiMobilePhotoAlbumTool 负责 Agent 工具调用与结果返回，本文件给出其实现。
// 直接依赖：MaiMobileHostTools.h；实现与这些接口协作。
#include "MaiMobileHostTools.h"

void registerPlatformPhotoAlbumTools(MaiToolRegistry& tools,
                                     const std::shared_ptr<MaiMobileHostDispatcher>& dispatcher) {
    addMaiMobileHostTool(tools,
        "mobile_photos_copy_to_album",
        "Android galleries use folders as albums. Copy the selected existing photos into "
        "Pictures/MaiChat/<album_name>; originals remain unchanged, so gallery duplicates "
        "will be visible.",
        R"({"type":"object","properties":{"album_name":{"type":"string"},"photo_ids":{"type":"array","items":{"type":"string"},"minItems":1,"maxItems":10}},"required":["album_name","photo_ids"]})",
        dispatcher, false);
}
