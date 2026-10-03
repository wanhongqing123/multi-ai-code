#include "MaiFaceImageCodecBridge.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "MaiImageDecode.h"
#include "MaiImageEncode.h"

bool maiDecodeFaceImage(const std::string &path, MaiFaceDecodedImage &image) {
    const MaiImageDecodeResult decoded = maiImageDecodeFile(path.c_str(), 0, 0);
    if (!decoded.rgba || decoded.width <= 0 || decoded.height <= 0 ||
        static_cast<std::int64_t>(decoded.width) * decoded.height > 24'000'000 ||
        decoded.stride < decoded.width * 4 || decoded.icc_size < 0) {
        maiImageDecodeFree(decoded.rgba);
        return false;
    }
    image.width = decoded.width;
    image.height = decoded.height;
    image.colorPrimaries = decoded.color_primaries;
    image.colorTransfer = decoded.color_transfer;
    image.rgba.resize(static_cast<std::size_t>(decoded.width) * decoded.height * 4);
    for (int row = 0; row < decoded.height; ++row)
        std::memcpy(image.rgba.data() + static_cast<std::size_t>(row) * decoded.width * 4,
                    decoded.rgba + static_cast<std::size_t>(row) * decoded.stride,
                    static_cast<std::size_t>(decoded.width) * 4);
    if (decoded.icc_profile && decoded.icc_size)
        image.iccProfile.assign(decoded.icc_profile, decoded.icc_profile + decoded.icc_size);
    maiImageDecodeFree(decoded.rgba);
    return true;
}

bool maiEncodeFacePng(const unsigned char *rgba, int width, int height, int stride,
                      const MaiFaceDecodedImage &source, std::vector<unsigned char> &png) {
    const MaiImageEncodeResult encoded = maiImageEncodePngRgba(
        rgba, width, height, stride, source.iccProfile.empty() ? nullptr : source.iccProfile.data(),
        source.iccProfile.size(), source.colorPrimaries, source.colorTransfer);
    if (!encoded.bytes || !encoded.size) {
        maiImageEncodeFree(encoded.bytes);
        return false;
    }
    png.assign(encoded.bytes, encoded.bytes + encoded.size);
    maiImageEncodeFree(encoded.bytes);
    return true;
}
