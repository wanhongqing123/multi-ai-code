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
