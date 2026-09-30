#include "MaiMobileHostTools.h"

void registerPlatformPhotoAlbumTools(MaiToolRegistry& tools,
                                     const std::shared_ptr<MaiMobileHostDispatcher>& dispatcher) {
    addMaiMobileHostTool(tools,
        "mobile_photos_add_to_album",
        "Create or reuse an iOS Photos album and add existing photo IDs to it without duplicating "
        "or removing originals.",
        R"({"type":"object","properties":{"album_name":{"type":"string"},"photo_ids":{"type":"array","items":{"type":"string"},"minItems":1,"maxItems":50}},"required":["album_name","photo_ids"]})",
        dispatcher, false);
}
