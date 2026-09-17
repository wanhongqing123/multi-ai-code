#include "MaiPathUtf8.h"

std::filesystem::path MaiPathUtf8::fromUtf8(const std::string& text) {
    return std::filesystem::u8path(text);
}

std::string MaiPathUtf8::toUtf8(const std::filesystem::path& path) {
    return path.u8string();
}

std::string MaiPathUtf8::toUtf8Generic(const std::filesystem::path& path) {
    return path.generic_u8string();
}
