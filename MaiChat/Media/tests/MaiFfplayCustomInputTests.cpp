#include "MaiFfplayHost.h"

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <iostream>

namespace {

struct FileSource {
    FILE* file = nullptr;
    uint64_t size = 0;
    uint64_t highestOffset = 0;
    unsigned reads = 0;
};

int readAt(void* user, uint64_t offset, uint8_t* output, int capacity) {
    auto* source = static_cast<FileSource*>(user);
    if (offset >= source->size) return 0;
    if (fseeko(source->file, static_cast<off_t>(offset), SEEK_SET) != 0) return -1;
    const size_t count = std::fread(output, 1, capacity, source->file);
    if (!count && std::ferror(source->file)) return -1;
    source->highestOffset = std::max(source->highestOffset, offset);
    ++source->reads;
    return static_cast<int>(count);
}

int check(const char* path, bool expectHlg) {
    FileSource source;
    source.file = std::fopen(path, "rb");
    if (!source.file) return 2;
    if (fseeko(source.file, 0, SEEK_END) != 0) return 3;
    source.size = static_cast<uint64_t>(ftello(source.file));
    MaiFfplayHost host{};
    host.user_data = &source;
    host.read_input_at = readAt;
    host.input_size_bytes = source.size;
    MaiFfplayHdrSourceInfo fromFile{};
    MaiFfplayHdrSourceInfo fromCustom{};
    const int fileOkay = maiFfplayProbeHdrSource(path, &fromFile);
    const int customOkay = maiFfplayProbeHdrSourceFromHost(&host, &fromCustom);
    std::fclose(source.file);
    if (fileOkay != 1 || customOkay != 1 || source.reads == 0 ||
        fromFile.native_frame_eligible != fromCustom.native_frame_eligible ||
        fromFile.hlg != fromCustom.hlg ||
        fromFile.has_ambient != fromCustom.has_ambient ||
        (fromFile.has_ambient &&
         std::memcmp(fromFile.ambient, fromCustom.ambient,
                     sizeof(fromFile.ambient)) != 0) ||
        (expectHlg && (!fromCustom.native_frame_eligible || !fromCustom.hlg))) {
        std::cerr << "custom AVIO HDR probe differs from local input: " << path << '\n';
        return 4;
    }
    std::cout << "custom AVIO probe: " << path << ", reads=" << source.reads
              << ", highest_offset=" << source.highestOffset
              << ", HLG=" << fromCustom.hlg << '\n';
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2 && argc != 3) return 1;
    const int first = check(argv[1], false);
    if (first) return first;
    return argc == 3 ? check(argv[2], true) : 0;
}
