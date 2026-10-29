#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif
#include "MaiScreenFrameArchive.h"
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <zlib.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {
using Json = nlohmann::json;
constexpr uint64_t kMaxPngBytes = 20'000'000;
constexpr uint64_t kBatchBytes = 256ULL * 1024 * 1024;
constexpr uint64_t kWorkspaceBytes = 512ULL * 1024 * 1024;
constexpr uint64_t kManifestReserve = 4096;
constexpr size_t kMaxManifest = 1024 * 1024;
constexpr int64_t kInterval = 2000;
std::mutex archiveMutex;

struct Failure : std::runtime_error {
    std::string code;
    Json details;
    Failure(std::string c, const char* message, Json extra = Json::object())
        : std::runtime_error(message), code(std::move(c)), details(std::move(extra)) {}
};
[[noreturn]] void invalid(const char* message) { throw Failure("invalid_input", message); }
[[noreturn]] void ioFailure() { throw Failure("internal", "screen archive file operation failed"); }
[[noreturn]] void limit(const char* reason) {
    throw Failure("limit", "screen archive limit reached; stop recording without deleting old frames",
                  {{"terminal", true}, {"limit_reason", reason}});
}
int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
bool textValid(const std::string& text, size_t max) {
    if (text.empty() || text.size() > max) return false;
    for (size_t i = 0; i < text.size();) {
        const unsigned char first = text[i++];
        uint32_t value = first;
        unsigned count = 0;
        uint32_t minimum = 0;
        if (first < 0x80) { if (first < 0x20 || first == 0x7f) return false; }
        else if (first >= 0xc2 && first <= 0xdf) { value &= 0x1f; count = 1; minimum = 0x80; }
        else if (first >= 0xe0 && first <= 0xef) { value &= 0x0f; count = 2; minimum = 0x800; }
        else if (first >= 0xf0 && first <= 0xf4) { value &= 7; count = 3; minimum = 0x10000; }
        else return false;
        if (i + count > text.size()) return false;
        while (count--) {
            const unsigned char next = text[i++];
            if ((next & 0xc0) != 0x80) return false;
            value = (value << 6) | (next & 0x3f);
        }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff) ||
            (value >= 0x80 && value <= 0x9f) || value == 0x2028 || value == 0x2029) return false;
    }
    return true;
}
std::string hex(const unsigned char* bytes, size_t size) {
    static const char digits[] = "0123456789abcdef";
    std::string result(size * 2, '0');
    for (size_t i = 0; i < size; ++i) {
        result[2 * i] = digits[bytes[i] >> 4]; result[2 * i + 1] = digits[bytes[i] & 15];
    }
    return result;
}
std::string hash(const void* data, size_t size) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned length = 0;
    if (EVP_Digest(data, size, digest.data(), &length, EVP_sha256(), nullptr) != 1 || length != 32)
        ioFailure();
    return hex(digest.data(), length);
}
std::string newID(const char* prefix) {
    std::array<unsigned char, 16> bytes{};
    if (RAND_bytes(bytes.data(), int(bytes.size())) != 1) ioFailure();
    return std::string(prefix) + hex(bytes.data(), bytes.size());
}
bool idValid(const std::string& id, const char* prefix) {
    const size_t n = std::strlen(prefix);
    return id.size() == n + 32 && id.compare(0, n, prefix) == 0 &&
        std::all_of(id.begin() + n, id.end(), [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        });
}
std::string join(const std::string& parent, const std::string& child) {
    return parent + (parent.empty() || parent.back() == '/' ? "" : "/") + child;
}
#ifdef _WIN32
std::wstring wide(const std::string& text) {
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), int(text.size()), nullptr, 0);
    if (!count) invalid("path is not valid UTF-8");
    std::wstring result(count, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), int(text.size()), result.data(), count);
    return result;
}
std::string utf8(const std::wstring& text) {
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr);
    if (!count) ioFailure();
    std::string result(count, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), int(text.size()), result.data(), count, nullptr, nullptr);
    return result;
}
#endif
struct Entry { bool exists = false; bool directory = false; uint64_t bytes = 0; };
Entry info(const std::string& path) {
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(wide(path).c_str(), GetFileExInfoStandard, &data)) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return {};
        ioFailure();
    }
    if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) invalid("archive paths must not be symbolic links");
    return {true, (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
            (uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow};
#else
    struct stat data{};
    if (lstat(path.c_str(), &data) != 0) { if (errno == ENOENT) return {}; ioFailure(); }
    if (S_ISLNK(data.st_mode) || (!S_ISDIR(data.st_mode) && !S_ISREG(data.st_mode)))
        invalid("archive paths must be regular files or directories without symbolic links");
    return {true, S_ISDIR(data.st_mode), uint64_t(data.st_size)};
#endif
}
void directory(const std::string& path) {
    auto entry = info(path);
    if (!entry.exists) {
#ifdef _WIN32
        if (!CreateDirectoryW(wide(path).c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) ioFailure();
#else
        if (mkdir(path.c_str(), 0700) != 0 && errno != EEXIST) ioFailure();
#endif
        entry = info(path);
    }
    if (!entry.exists || !entry.directory) invalid("archive directory is not a directory");
}
std::string canonicalWorkspace(const char* value) {
    if (!value) invalid("workspace is required");
    const std::string path(value);
    if (!textValid(path, 32768)) invalid("workspace is not a bounded UTF-8 path");
#ifdef _WIN32
    if (!(path[0] == '/' || path[0] == '\\' || (path.size() > 2 && path[1] == ':')))
        invalid("workspace must be absolute");
    HANDLE handle = CreateFileW(wide(path).c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) invalid("workspace must already exist");
    const DWORD count = GetFinalPathNameByHandleW(handle, nullptr, 0, FILE_NAME_NORMALIZED);
    std::wstring resolved(count + 1, L'\0');
    const DWORD written = GetFinalPathNameByHandleW(handle, resolved.data(), DWORD(resolved.size()), FILE_NAME_NORMALIZED);
    CloseHandle(handle);
    if (!written || written >= resolved.size()) ioFailure();
    resolved.resize(written);
    if (resolved.rfind(L"\\\\?\\UNC\\", 0) == 0) resolved = L"\\\\" + resolved.substr(8);
    else if (resolved.rfind(L"\\\\?\\", 0) == 0) resolved.erase(0, 4);
    std::string result = utf8(resolved);
    std::replace(result.begin(), result.end(), '\\', '/');
#else
    if (path[0] != '/') invalid("workspace must be absolute");
    char* resolved = realpath(path.c_str(), nullptr);
    if (!resolved) invalid("workspace must already exist");
    std::string result(resolved); std::free(resolved);
#endif
    while (result.size() > 1 && result.back() == '/') {
#ifdef _WIN32
        if (result.size() == 3 && result[1] == ':') break;
#endif
        result.pop_back();
    }
    if (!info(result).directory) invalid("workspace must be a directory");
    return result;
}
std::vector<std::string> children(const std::string& path) {
    std::vector<std::string> names;
    if (!info(path).exists) return names;
#ifdef _WIN32
    WIN32_FIND_DATAW data{};
    HANDLE search = FindFirstFileW(wide(join(path, "*")).c_str(), &data);
    if (search == INVALID_HANDLE_VALUE) { if (GetLastError() == ERROR_FILE_NOT_FOUND) return names; ioFailure(); }
    do {
        const std::wstring name(data.cFileName);
        if (name != L"." && name != L"..") names.push_back(utf8(name));
    } while (FindNextFileW(search, &data));
    const DWORD error = GetLastError(); FindClose(search);
    if (error != ERROR_NO_MORE_FILES) ioFailure();
#else
    DIR* stream = opendir(path.c_str());
    if (!stream) ioFailure();
    errno = 0;
    while (dirent* entry = readdir(stream)) {
        const std::string name(entry->d_name);
        if (name != "." && name != "..") names.push_back(name);
        errno = 0;
    }
    const int error = errno; closedir(stream);
    if (error) ioFailure();
#endif
    std::sort(names.begin(), names.end());
    return names;
}
void eraseFile(const std::string& path) {
    if (!info(path).exists) return;
#ifdef _WIN32
    if (!DeleteFileW(wide(path).c_str())) ioFailure();
#else
    if (unlink(path.c_str()) != 0) ioFailure();
#endif
}
void eraseTree(const std::string& path) {
    const auto entry = info(path);
    if (!entry.exists) return;
    if (!entry.directory) { eraseFile(path); return; }
    for (const auto& name : children(path)) eraseTree(join(path, name));
#ifdef _WIN32
    if (!RemoveDirectoryW(wide(path).c_str())) ioFailure();
#else
    if (rmdir(path.c_str()) != 0) ioFailure();
#endif
}
std::string readFile(const std::string& path, uint64_t max) {
    const auto entry = info(path);
    if (!entry.exists || entry.directory) throw Failure("not_found", "archive file was not found");
    if (entry.bytes > max) invalid("archive file exceeds its bound");
    std::string result(size_t(entry.bytes), '\0');
#ifdef _WIN32
    HANDLE file = CreateFileW(wide(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) ioFailure();
    DWORD read = 0;
    const bool ok = ReadFile(file, result.data(), DWORD(result.size()), &read, nullptr) && read == result.size();
    CloseHandle(file);
#else
    const int file = open(path.c_str(), O_RDONLY | O_NOFOLLOW);
    if (file < 0) ioFailure();
    size_t count = 0;
    while (count < result.size()) {
        const ssize_t n = read(file, result.data() + count, result.size() - count);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        count += size_t(n);
    }
    const bool ok = count == result.size(); close(file);
#endif
    if (!ok) ioFailure();
    return result;
}
void atomicWrite(const std::string& path, const void* bytes, size_t size, bool replace) {
    const std::string temp = path + ".tmp_" + newID("");
#ifdef _WIN32
    HANDLE file = CreateFileW(wide(temp).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) ioFailure();
    DWORD written = 0;
    const bool ok = WriteFile(file, bytes, DWORD(size), &written, nullptr) && written == size && FlushFileBuffers(file);
    CloseHandle(file);
    if (!ok) { DeleteFileW(wide(temp).c_str()); ioFailure(); }
    if (!MoveFileExW(wide(temp).c_str(), wide(path).c_str(),
                     MOVEFILE_WRITE_THROUGH | (replace ? MOVEFILE_REPLACE_EXISTING : 0))) {
        DeleteFileW(wide(temp).c_str()); ioFailure();
    }
#else
    const int file = open(temp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (file < 0) ioFailure();
    size_t count = 0;
    while (count < size) {
        const ssize_t n = write(file, static_cast<const char*>(bytes) + count, size - count);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        count += size_t(n);
    }
    const bool ok = count == size && fsync(file) == 0;
    close(file);
    if (!ok) { unlink(temp.c_str()); ioFailure(); }
    const int committed = replace ? rename(temp.c_str(), path.c_str()) : link(temp.c_str(), path.c_str());
    if (committed != 0) { unlink(temp.c_str()); ioFailure(); }
    if (!replace) unlink(temp.c_str());
    const auto slash = path.find_last_of('/');
    const int parent = open(path.substr(0, slash).c_str(), O_RDONLY | O_DIRECTORY);
    if (parent >= 0) { fsync(parent); close(parent); }
#endif
}
struct WorkspaceLock {
#ifdef _WIN32
    HANDLE handle = INVALID_HANDLE_VALUE;
    OVERLAPPED state{};
#else
    int handle = -1;
#endif
    explicit WorkspaceLock(const std::string& root) {
        const std::string path = join(root, ".lock");
        const auto entry = info(path);
        if (entry.exists && entry.directory) invalid("archive lock is invalid");
#ifdef _WIN32
        handle = CreateFileW(wide(path).c_str(), GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
            FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (handle == INVALID_HANDLE_VALUE) ioFailure();
        if (!LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK, 0, 1, 0, &state)) {
            CloseHandle(handle); handle = INVALID_HANDLE_VALUE; ioFailure();
        }
#else
        handle = open(path.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW, 0600);
        if (handle < 0) ioFailure();
        if (flock(handle, LOCK_EX) != 0) { close(handle); handle = -1; ioFailure(); }
#endif
    }
    ~WorkspaceLock() {
#ifdef _WIN32
        if (handle != INVALID_HANDLE_VALUE) { UnlockFileEx(handle, 0, 1, 0, &state); CloseHandle(handle); }
#else
        if (handle >= 0) { flock(handle, LOCK_UN); close(handle); }
#endif
    }
};
uint32_t be32(const unsigned char* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
struct PngSize { uint32_t width; uint32_t height; };
PngSize validatePNG(const void* bytes, size_t size) {
    static const unsigned char signature[] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (!bytes || size < 57 || size > kMaxPngBytes || std::memcmp(bytes, signature, 8) != 0)
        invalid("a complete bounded PNG is required");
    const auto* data = static_cast<const unsigned char*>(bytes);
    uint32_t width = 0, height = 0;
    unsigned depth = 0, color = 0, channels = 0, interlace = 0;
    bool ihdr = false, idat = false, afterIdat = false, end = false, palette = false;
    std::vector<std::pair<const unsigned char*, size_t>> compressed;
    for (size_t offset = 8; offset < size;) {
        if (size - offset < 12) invalid("PNG chunk is truncated");
        const uint32_t length = be32(data + offset);
        if (uint64_t(length) + 12 > size - offset) invalid("PNG chunk exceeds file bounds");
        const unsigned char* type = data + offset + 4;
        const unsigned char* body = type + 4;
        for (unsigned i = 0; i < 4; ++i)
            if (!((type[i] >= 'A' && type[i] <= 'Z') || (type[i] >= 'a' && type[i] <= 'z')))
                invalid("PNG chunk type is invalid");
        uLong crc = crc32(0, type, 4);
        crc = crc32(crc, body, length);
        if (uint32_t(crc) != be32(body + length)) invalid("PNG checksum mismatch");
        const std::string name(reinterpret_cast<const char*>(type), 4);
        if (!ihdr && name != "IHDR") invalid("PNG must start with IHDR");
        if (name == "IHDR") {
            if (ihdr || length != 13) invalid("PNG IHDR is invalid");
            ihdr = true; width = be32(body); height = be32(body + 4);
            depth = body[8]; color = body[9]; interlace = body[12];
            if (!width || !height || width > 16384 || height > 16384 ||
                uint64_t(width) * height > 64ULL * 1024 * 1024 ||
                body[10] != 0 || body[11] != 0 || interlace > 1)
                invalid("PNG dimensions or encoding are unsupported");
            if (color == 0) channels = 1;
            else if (color == 2) channels = 3;
            else if (color == 3) channels = 1;
            else if (color == 4) channels = 2;
            else if (color == 6) channels = 4;
            else invalid("PNG color type is invalid");
            const bool validDepth = color == 0 ? (depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16) :
                color == 3 ? (depth == 1 || depth == 2 || depth == 4 || depth == 8) : (depth == 8 || depth == 16);
            if (!validDepth) invalid("PNG bit depth is invalid");
        } else if (name == "PLTE") {
            if (palette || idat || !length || length % 3 || length > 768 || color == 0 || color == 4 ||
                (color == 3 && length / 3 > (1U << depth))) invalid("PNG palette is invalid");
            palette = true;
        } else if (name == "IDAT") {
            if (afterIdat || (color == 3 && !palette)) invalid("PNG image chunks are invalid");
            idat = true; compressed.emplace_back(body, length);
        } else if (name == "IEND") {
            if (length || !idat || offset + 12 != size) invalid("PNG end is invalid");
            end = true;
        } else if ((type[0] & 32) == 0) invalid("PNG has an unsupported critical chunk");
        if (idat && name != "IDAT") afterIdat = true;
        offset += size_t(length) + 12;
    }
    if (!end) invalid("PNG has no complete end");
    struct Pass { uint64_t rowBytes; uint32_t rows; };
    std::vector<Pass> passes;
    const auto addPass = [&](uint32_t w, uint32_t h) {
        if (w && h) passes.push_back({(uint64_t(w) * channels * depth + 7) / 8, h});
    };
    if (!interlace) addPass(width, height);
    else {
        constexpr unsigned x[] = {0, 4, 0, 2, 0, 1, 0}, y[] = {0, 0, 4, 0, 2, 0, 1};
        constexpr unsigned dx[] = {8, 8, 4, 4, 2, 2, 1}, dy[] = {8, 8, 8, 4, 4, 2, 2};
        for (unsigned i = 0; i < 7; ++i)
            addPass(width <= x[i] ? 0 : (width - x[i] + dx[i] - 1) / dx[i],
                    height <= y[i] ? 0 : (height - y[i] + dy[i] - 1) / dy[i]);
    }
    size_t pass = 0;
    uint32_t row = 0;
    uint64_t remaining = passes[0].rowBytes + 1;
    z_stream stream{};
    if (inflateInit(&stream) != Z_OK) ioFailure();
    bool streamEnd = false;
    try {
        std::array<unsigned char, 32768> output{};
        for (const auto& chunk : compressed) {
            if (streamEnd && chunk.second) invalid("PNG image stream has trailing data");
            stream.next_in = const_cast<Bytef*>(chunk.first);
            stream.avail_in = uInt(chunk.second);
            bool more = stream.avail_in != 0;
            while (more) {
                stream.next_out = output.data(); stream.avail_out = uInt(output.size());
                const int status = inflate(&stream, Z_NO_FLUSH);
                size_t produced = output.size() - stream.avail_out, offset = 0;
                while (offset < produced) {
                    if (pass >= passes.size()) invalid("PNG expands beyond its dimensions");
                    if (remaining == passes[pass].rowBytes + 1 && output[offset] > 4)
                        invalid("PNG row filter is invalid");
                    const size_t consume = size_t((std::min)(remaining, uint64_t(produced - offset)));
                    offset += consume; remaining -= consume;
                    if (!remaining) {
                        if (++row == passes[pass].rows) { ++pass; row = 0; }
                        if (pass < passes.size()) remaining = passes[pass].rowBytes + 1;
                    }
                }
                if (status == Z_STREAM_END) { streamEnd = true; if (stream.avail_in) invalid("PNG stream has trailing data"); break; }
                if (status == Z_BUF_ERROR && !stream.avail_in && !produced) break;
                if (status != Z_OK) invalid("PNG compressed pixels are invalid");
                more = stream.avail_in != 0 || stream.avail_out == 0;
            }
        }
        if (!streamEnd || pass != passes.size()) invalid("PNG compressed pixels are incomplete");
    } catch (...) { inflateEnd(&stream); throw; }
    inflateEnd(&stream);
    return {width, height};
}
int64_t integer(const Json& object, const char* key, int64_t minimum, int64_t maximum) {
    if (!object.contains(key) || !object[key].is_number_integer()) invalid("an integer metadata field is missing");
    if (object[key].is_number_unsigned() && object[key].get<uint64_t>() > uint64_t(maximum))
        invalid("an integer metadata field exceeds its bound");
    const int64_t result = object[key].get<int64_t>();
    if (result < minimum || result > maximum) invalid("an integer metadata field exceeds its bound");
    return result;
}
void fields(const Json& value, std::initializer_list<const char*> allowed) {
    if (!value.is_object()) invalid("request must be an object");
    for (auto item = value.begin(); item != value.end(); ++item) {
        bool found = false;
        for (const char* key : allowed) if (item.key() == key) found = true;
        if (!found) invalid("request contains an unknown field");
    }
}
std::string text(const Json& value, const char* key) {
    if (!value.contains(key) || !value[key].is_string()) invalid("a string field is missing");
    return value[key].get<std::string>();
}
Json batchPublic(const Json& manifest) {
    return {{"id", manifest["id"]}, {"status", manifest["status"]},
        {"frame_count", manifest["frames"].size()}, {"total_bytes", manifest["total_bytes"]},
        {"started_at_ms", manifest["started_at_ms"]}, {"finished_at_ms", manifest["finished_at_ms"]},
        {"finish_reason", manifest["finish_reason"]}};
}
Json framePublic(Json frame) {
    frame.erase("sha256"); frame.erase("saved_at_ms"); frame.erase("references"); return frame;
}
struct Archive {
    std::string root;
    std::string owner;
    std::string ownerDirectory;
    std::string relative;
    std::string base;
    explicit Archive(const char* workspace, const char* session) : root(canonicalWorkspace(workspace)) {
        if (!session || !textValid(session, 512)) invalid("owner_session_id must be bounded nonempty UTF-8");
        owner = session;
        const std::string metadata = join(root, ".maichat"); directory(metadata);
        base = join(metadata, "screen-frame-archive"); directory(base);
        ownerDirectory = "owner_" + hash(owner.data(), owner.size());
        relative = ".maichat/screen-frame-archive/" + ownerDirectory;
    }
    std::string ownerPath() const { return join(base, ownerDirectory); }
    std::string batchPath(const std::string& id) const {
        if (!idValid(id, "batch_")) invalid("batch_id is invalid");
        return join(ownerPath(), id);
    }
    std::string frameRelative(const std::string& batch, const std::string& frame) const {
        if (!idValid(batch, "batch_") || !idValid(frame, "frame_")) invalid("archive ID is invalid");
        return relative + "/" + batch + "/" + frame + ".png";
    }
    Json load(const std::string& id, bool checkFiles = true) const {
        const auto parent = info(ownerPath());
        if (!parent.exists) throw Failure("not_found", "screen frame batch was not found in this session");
        if (!parent.directory) invalid("archive owner directory is invalid");
        const std::string folder = batchPath(id);
        if (!info(folder).exists) throw Failure("not_found", "screen frame batch was not found in this session");
        if (!info(folder).directory) invalid("archive batch is not a directory");
        const std::string bytes = readFile(join(folder, "manifest.json"), kMaxManifest);
        Json value = Json::parse(bytes, nullptr, false);
        if (!value.is_object() || value.value("version", 0) != 1 || value.value("id", "") != id ||
            value.value("owner_session_id", "") != owner ||
            value.value("workspace_key", "") != hash(root.data(), root.size()) ||
            !value.contains("frames") || !value["frames"].is_array() || value["frames"].size() > 300)
            invalid("archive manifest ownership or structure is invalid");
        const std::string status = value.value("status", "");
        if (status != "recording" && status != "stopped" && status != "retained")
            invalid("archive batch state is invalid");
        integer(value, "started_at_ms", 1, 9'007'199'254'740'991LL);
        if (!value.contains("finished_at_ms") || !value.contains("finish_reason")) invalid("archive batch metadata is missing");
        if (status == "recording") {
            if (!value["finished_at_ms"].is_null() || !value["finish_reason"].is_null()) invalid("recording metadata is invalid");
        } else {
            integer(value, "finished_at_ms", 1, 9'007'199'254'740'991LL);
            if (!value["finish_reason"].is_string() || !textValid(value["finish_reason"], 256)) invalid("stop reason is invalid");
        }
        uint64_t total = 0;
        int64_t elapsed = -kInterval;
        std::vector<std::string> ids;
        size_t referenceCount = 0;
        for (auto& frame : value["frames"]) {
            const std::string frameID = text(frame, "id");
            if (!idValid(frameID, "frame_") || text(frame, "batch_id") != id ||
                text(frame, "path") != frameRelative(id, frameID) ||
                std::find(ids.begin(), ids.end(), frameID) != ids.end()) invalid("frame identity is invalid");
            ids.push_back(frameID);
            const auto bytes = integer(frame, "size_bytes", 1, kMaxPngBytes);
            if (checkFiles) {
                const auto item = info(join(root, text(frame, "path")));
                if (!item.exists || item.directory || item.bytes != uint64_t(bytes))
                    invalid("archived PNG is missing or has changed");
            }
            total += uint64_t(bytes);
            integer(frame, "width", 1, 16384); integer(frame, "height", 1, 16384);
            integer(frame, "captured_at_ms", 1, 9'007'199'254'740'991LL);
            integer(frame, "saved_at_ms", 1, 9'007'199'254'740'991LL);
            const auto next = integer(frame, "elapsed_ms", 0, 9'007'199'254'740'991LL);
            if (next < elapsed + kInterval) invalid("frame intervals in manifest are invalid");
            elapsed = next;
            const auto sha = text(frame, "sha256");
            if (sha.size() != 64 || !std::all_of(sha.begin(), sha.end(), [](char c) {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); })) invalid("frame digest is invalid");
            if (!frame.contains("references")) frame["references"] = Json::array();
            if (!frame["references"].is_array()) invalid("frame references must be an array");
            referenceCount += frame["references"].size();
            if (referenceCount > 4096) invalid("archive has too many references");
            std::vector<std::string> references;
            for (const auto& reference : frame["references"]) {
                fields(reference, {"id", "usage_session_id"});
                const std::string token = text(reference, "id");
                if (!textValid(token, 128) || !textValid(text(reference, "usage_session_id"), 512) ||
                    std::find(references.begin(), references.end(), token) != references.end())
                    invalid("frame reference identity is invalid");
                references.push_back(token);
            }
        }
        if (total > kBatchBytes || integer(value, "total_bytes", 0, kBatchBytes) != int64_t(total))
            invalid("archive batch byte count is invalid");
        return value;
    }
    uint64_t occupied(const std::string& path, unsigned depth = 0) const {
        if (depth > 4) invalid("archive directory nesting is invalid");
        uint64_t total = 0;
        for (const auto& name : children(path)) {
            const auto child = join(path, name);
            const auto entry = info(child);
            if (entry.directory) {
                const auto bytes = occupied(child, depth + 1);
                if (bytes > kWorkspaceBytes - total) return kWorkspaceBytes + 1;
                total += bytes;
            }
            else {
                if (entry.bytes > kWorkspaceBytes - total) return kWorkspaceBytes + 1;
                total += entry.bytes;
                if (name == "manifest.json") {
                    const auto parsed = Json::parse(readFile(child, kMaxManifest), nullptr, false);
                    if (parsed.is_object() && parsed.value("status", "") == "recording") {
                        if (kManifestReserve > kWorkspaceBytes - total) return kWorkspaceBytes + 1;
                        total += kManifestReserve;
                    }
                }
            }
            if (total > kWorkspaceBytes) return total;
        }
        return total;
    }
    void save(const std::string& id, const Json& value) const {
        const std::string bytes = value.dump();
        if (bytes.size() > kMaxManifest) invalid("archive manifest is too large");
        const std::string path = join(batchPath(id), "manifest.json");
        int64_t previousBytes = 0;
        if (info(path).exists) {
            const std::string previous = readFile(path, kMaxManifest);
            previousBytes = int64_t(previous.size());
            const Json parsed = Json::parse(previous, nullptr, false);
            if (parsed.is_object() && parsed.value("status", "") == "recording")
                previousBytes += kManifestReserve;
        }
        const int64_t nextBytes = int64_t(bytes.size()) +
            (value["status"] == "recording" ? int64_t(kManifestReserve) : 0);
        if (nextBytes > previousBytes &&
            archiveBytesExceeded(uint64_t(nextBytes - previousBytes))) limit("workspace_bytes");
        atomicWrite(path, bytes.data(), bytes.size(), true);
    }
    bool archiveBytesExceeded(uint64_t additional) const {
        const uint64_t used = occupied(base);
        return used > kWorkspaceBytes || additional > kWorkspaceBytes - used;
    }
};
Json checkedFrame(const Archive& archive, const Json& frame) {
    const std::string bytes = readFile(join(archive.root, text(frame, "path")), kMaxPngBytes);
    if (bytes.size() != uint64_t(frame["size_bytes"]) || hash(bytes.data(), bytes.size()) != frame["sha256"])
        invalid("archived PNG content no longer matches its manifest");
    const auto size = validatePNG(bytes.data(), bytes.size());
    if (size.width != uint64_t(frame["width"]) || size.height != uint64_t(frame["height"]))
        invalid("archived PNG dimensions no longer match metadata");
    return framePublic(frame);
}
struct Removal { size_t frames = 0; size_t batches = 0; };
Removal saveReferences(const Archive& archive, Json& manifest) {
    const std::string id = manifest["id"];
    Removal removed;
    if (manifest["status"] != "retained") {
        archive.save(id, manifest);
        return removed;
    }
    Json retained = Json::array();
    uint64_t total = 0;
    for (const auto& frame : manifest["frames"]) {
        if (frame["references"].empty()) { ++removed.frames; continue; }
        retained.push_back(frame); total += uint64_t(frame["size_bytes"]);
    }
    if (retained.empty()) {
        eraseTree(archive.batchPath(id)); removed.batches = 1; return removed;
    }
    manifest["frames"] = std::move(retained); manifest["total_bytes"] = total;
    // Publish the protected set before unlinking anything. A crash can leave
    // unreferenced files, but never remove a PNG still named by a reference.
    archive.save(id, manifest);
    for (const auto& name : children(archive.batchPath(id))) {
        if (name.size() <= 4 || name.substr(name.size() - 4) != ".png" ||
            !idValid(name.substr(0, name.size() - 4), "frame_")) continue;
        bool protectedFile = false;
        for (const auto& frame : manifest["frames"])
            if (name == frame["id"].get<std::string>() + ".png") protectedFile = true;
        if (!protectedFile) eraseFile(join(archive.batchPath(id), name));
    }
    return removed;
}
Json releaseSession(const Archive& archive) {
    size_t references = 0, frames = 0, batches = 0;
    for (const auto& ownerDirectory : children(archive.base)) {
        if (ownerDirectory.size() != 70 || ownerDirectory.rfind("owner_", 0) != 0 ||
            !std::all_of(ownerDirectory.begin() + 6, ownerDirectory.end(), [](char c) {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); })) continue;
        const std::string path = join(archive.base, ownerDirectory);
        if (!info(path).directory) invalid("archive owner directory is invalid");
        for (const auto& id : children(path)) {
            if (!idValid(id, "batch_")) continue;
            const Json header = Json::parse(readFile(join(join(path, id), "manifest.json"), kMaxManifest), nullptr, false);
            const std::string owner = text(header, "owner_session_id");
            if (!textValid(owner, 512) || "owner_" + hash(owner.data(), owner.size()) != ownerDirectory)
                invalid("archive owner directory does not match its manifest");
            Archive scoped(archive.root.c_str(), owner.c_str());
            Json manifest = scoped.load(id, false);
            bool changed = false;
            for (auto& frame : manifest["frames"]) {
                auto& refs = frame["references"];
                for (auto ref = refs.begin(); ref != refs.end();) {
                    if ((*ref)["usage_session_id"] == archive.owner) {
                        ref = refs.erase(ref); ++references; changed = true;
                    } else ++ref;
                }
            }
            if (changed) {
                const auto removed = saveReferences(scoped, manifest);
                frames += removed.frames; batches += removed.batches;
            }
        }
    }
    return {{"released_references", references}, {"removed_frames", frames}, {"removed_batches", batches}};
}
Json perform(const char* workspace, const char* owner, const Json& request,
             const void* png, size_t pngSize) {
    const std::string action = text(request, "action");
    if (action != "begin" && action != "append" && action != "finish" &&
        action != "list" && action != "get" && action != "discard" &&
        action != "retain" && action != "release" && action != "assign_session" &&
        action != "release_session")
        invalid("unknown screen archive action");
    if (action != "append" && (png || pngSize)) invalid("PNG bytes are accepted only by append");
    Archive archive(workspace, owner);
    WorkspaceLock workspaceLock(archive.base);
    if (action == "release_session") {
        fields(request, {"action"}); return releaseSession(archive);
    }
    if (action == "begin") {
        fields(request, {"action"});
        const std::string id = newID("batch_");
        Json manifest{{"version", 1}, {"id", id}, {"owner_session_id", archive.owner},
            {"workspace_key", hash(archive.root.data(), archive.root.size())}, {"status", "recording"},
            {"started_at_ms", nowMs()}, {"finished_at_ms", nullptr}, {"finish_reason", nullptr},
            {"total_bytes", 0}, {"frames", Json::array()}};
        if (archive.occupied(archive.base) + manifest.dump().size() + kManifestReserve > kWorkspaceBytes)
            limit("workspace_bytes");
        directory(archive.ownerPath());
        directory(archive.batchPath(id));
        try { archive.save(id, manifest); }
        catch (...) { eraseTree(archive.batchPath(id)); throw; }
        return {{"batch_id", id}, {"batch", batchPublic(manifest)}};
    }
    if (action == "list") {
        fields(request, {"action", "batch_id"});
        Json batches = Json::array(), frames = Json::array();
        std::vector<Json> manifests;
        if (request.contains("batch_id")) manifests.push_back(archive.load(text(request, "batch_id")));
        else {
            for (const auto& name : children(archive.ownerPath()))
                if (idValid(name, "batch_")) manifests.push_back(archive.load(name));
        }
        std::sort(manifests.begin(), manifests.end(), [](const Json& a, const Json& b) {
            if (a["started_at_ms"] != b["started_at_ms"]) return a["started_at_ms"] > b["started_at_ms"];
            return a["id"] < b["id"];
        });
        for (const auto& manifest : manifests) {
            if (manifest["status"] == "retained") continue;
            batches.push_back(batchPublic(manifest));
            for (const auto& frame : manifest["frames"]) frames.push_back(framePublic(frame));
        }
        return {{"batches", batches}, {"frames", frames}};
    }
    const std::string id = text(request, "batch_id");
    if (action == "release") fields(request, {"action", "batch_id", "frame_id", "reference_id"});
    Json manifest;
    try { manifest = archive.load(id, action != "discard" && action != "release" && action != "assign_session"); }
    catch (const Failure& failure) {
        if (action != "release" || failure.code != "not_found") throw;
        const std::string frameID = text(request, "frame_id"), token = text(request, "reference_id");
        if (!idValid(frameID, "frame_") || !textValid(token, 128)) invalid("frame reference identity is invalid");
        return {{"batch_id", id}, {"frame_id", frameID}, {"reference_id", token},
            {"released", false}, {"removed_frames", 0}, {"removed_batches", 0}};
    }
    if (action == "append") {
        fields(request, {"action", "batch_id", "width", "height", "captured_at_ms", "elapsed_ms"});
        if (manifest["status"] != "recording") invalid("cannot append to a stopped screen batch");
        if (!png || !pngSize || pngSize > kMaxPngBytes) invalid("a bounded PNG is required");
        const int64_t width = integer(request, "width", 1, 16384);
        const int64_t height = integer(request, "height", 1, 16384);
        const int64_t captured = integer(request, "captured_at_ms", 1, 9'007'199'254'740'991LL);
        const int64_t elapsed = integer(request, "elapsed_ms", 0, 9'007'199'254'740'991LL);
        auto& frames = manifest["frames"];
        if (frames.size() >= 300) limit("batch_frames");
        if (uint64_t(manifest["total_bytes"]) + pngSize > kBatchBytes) limit("batch_bytes");
        const int64_t saved = nowMs();
        if (!frames.empty()) {
            const auto& previous = frames.back();
            const int64_t remainingElapsed = kInterval - (elapsed - int64_t(previous["elapsed_ms"]));
            const int64_t remainingReal = kInterval - (saved - int64_t(previous["saved_at_ms"]));
            const int64_t retry = (std::max)(remainingElapsed, remainingReal);
            if (retry > 0) throw Failure("rate_limited", "screen frames must be at least 2000 ms apart",
                {{"terminal", false}, {"retry_after_ms", retry}, {"min_interval_ms", kInterval}});
        }
        const PngSize dimensions = validatePNG(png, pngSize);
        if (dimensions.width != width || dimensions.height != height) invalid("PNG dimensions do not match metadata");
        const std::string frameID = newID("frame_");
        const std::string relative = archive.frameRelative(id, frameID);
        Json frame{{"id", frameID}, {"batch_id", id}, {"captured_at_ms", captured},
            {"elapsed_ms", elapsed}, {"saved_at_ms", saved}, {"size_bytes", pngSize},
            {"width", width}, {"height", height}, {"path", relative}, {"sha256", hash(png, pngSize)},
            {"references", Json::array()}};
        const size_t oldManifestSize = manifest.dump().size();
        frames.push_back(frame);
        manifest["total_bytes"] = uint64_t(manifest["total_bytes"]) + pngSize;
        const uint64_t projected = archive.occupied(archive.base) + pngSize + manifest.dump().size() - oldManifestSize;
        if (projected > kWorkspaceBytes) limit("workspace_bytes");
        const std::string path = join(archive.root, relative);
        atomicWrite(path, png, pngSize, false);
        try { archive.save(id, manifest); }
        catch (...) { eraseFile(path); throw; }
        const char* reason = frames.size() >= 300 ? "batch_frames" :
            uint64_t(manifest["total_bytes"]) >= kBatchBytes ? "batch_bytes" :
            projected >= kWorkspaceBytes ? "workspace_bytes" : nullptr;
        Json result{{"batch_id", id}, {"batch", batchPublic(manifest)},
            {"frame", framePublic(frame)}, {"limit_reached", reason != nullptr}};
        if (reason) result["limit_reason"] = reason;
        return result;
    }
    if (action == "finish") {
        fields(request, {"action", "batch_id", "reason"});
        const std::string reason = request.contains("reason") ? text(request, "reason") : "user_stopped";
        if (!textValid(reason, 256)) invalid("finish reason must be bounded UTF-8");
        if (manifest["status"] == "recording") {
            manifest["status"] = "stopped";
            manifest["finished_at_ms"] = nowMs();
            manifest["finish_reason"] = reason;
            archive.save(id, manifest);
        }
        return {{"batch_id", id}, {"batch", batchPublic(manifest)}};
    }
    if (action == "get") {
        fields(request, {"action", "batch_id", "frame_id"});
        const std::string frameID = text(request, "frame_id");
        if (!idValid(frameID, "frame_")) invalid("frame_id is invalid");
        for (const auto& frame : manifest["frames"]) {
            if (frame["id"] != frameID) continue;
            return checkedFrame(archive, frame);
        }
        throw Failure("not_found", "screen frame was not found in this session batch");
    }
    if (action == "retain" || action == "release" || action == "assign_session") {
        if (action == "retain" || action == "assign_session")
            fields(request, {"action", "batch_id", "frame_id", "reference_id", "usage_session_id"});
        const std::string frameID = text(request, "frame_id"), token = text(request, "reference_id");
        if (!idValid(frameID, "frame_") || !textValid(token, 128)) invalid("frame reference identity is invalid");
        const std::string usage = action == "release" ? std::string() :
            request.contains("usage_session_id") ? text(request, "usage_session_id") :
            action == "retain" ? archive.owner : std::string();
        if (action != "release" && !textValid(usage, 512)) invalid("usage_session_id must be bounded UTF-8");
        for (auto& frame : manifest["frames"]) {
            if (frame["id"] != frameID) continue;
            auto& refs = frame["references"];
            auto found = std::find_if(refs.begin(), refs.end(), [&](const Json& ref) { return ref["id"] == token; });
            if (action == "retain") {
                Json output = checkedFrame(archive, frame);
                if (found != refs.end() && (*found)["usage_session_id"] != usage)
                    invalid("reference token already belongs to another usage session");
                if (found == refs.end()) {
                    size_t total = 0;
                    for (const auto& item : manifest["frames"]) total += item["references"].size();
                    if (total >= 4096) throw Failure("limit", "screen archive reference limit reached",
                        {{"terminal", false}, {"limit_reason", "reference_tokens"}});
                    refs.push_back({{"id", token}, {"usage_session_id", usage}}); archive.save(id, manifest);
                }
                output["reference_id"] = token; output["retained"] = true; return output;
            }
            if (action == "assign_session") {
                if (found == refs.end()) throw Failure("not_found", "screen frame reference was not found");
                const bool changed = (*found)["usage_session_id"] != usage;
                if (changed) { (*found)["usage_session_id"] = usage; archive.save(id, manifest); }
                return {{"batch_id", id}, {"frame_id", frameID}, {"reference_id", token}, {"updated", changed}};
            }
            const bool released = found != refs.end();
            if (released) refs.erase(found);
            const auto removed = released || manifest["status"] == "retained" ?
                saveReferences(archive, manifest) : Removal{};
            return {{"batch_id", id}, {"frame_id", frameID}, {"reference_id", token},
                {"released", released}, {"removed_frames", removed.frames}, {"removed_batches", removed.batches}};
        }
        if (action == "release") {
            const auto removed = manifest["status"] == "retained" ?
                saveReferences(archive, manifest) : Removal{};
            return {{"batch_id", id}, {"frame_id", frameID}, {"reference_id", token},
                {"released", false}, {"removed_frames", removed.frames}, {"removed_batches", removed.batches}};
        }
        throw Failure("not_found", "screen frame was not found in this session batch");
    }
    if (action == "discard") {
        fields(request, {"action", "batch_id"});
        manifest["status"] = "retained";
        if (manifest["finished_at_ms"].is_null()) {
            manifest["finished_at_ms"] = nowMs(); manifest["finish_reason"] = "discarded";
        }
        const auto removed = saveReferences(archive, manifest);
        return {{"batch_id", id}, {"discarded", true}, {"removed_frames", removed.frames},
            {"retained_frames", removed.batches ? size_t(0) : manifest["frames"].size()}};
    }
    invalid("unknown screen archive action");
}
char* owned(const Json& result) {
    const std::string json = result.dump();
    auto* output = static_cast<char*>(std::malloc(json.size() + 1));
    if (output) std::memcpy(output, json.c_str(), json.size() + 1);
    return output;
}
} // namespace

extern "C" char* mai_screen_frame_archive_request(const char* workspace,
    const char* owner, const char* request, const void* png, size_t pngSize) {
  try {
    try {
        if (!request || std::strlen(request) > 16384) invalid("request JSON is missing or too large");
        const Json parsed = Json::parse(request, nullptr, false);
        if (!parsed.is_object()) invalid("request must be a JSON object");
        std::lock_guard<std::mutex> guard(archiveMutex);
        return owned({{"ok", true}, {"output", perform(workspace, owner, parsed, png, pngSize)}});
    } catch (const Failure& failure) {
        Json result = failure.details;
        result["ok"] = false; result["errorCode"] = failure.code; result["error"] = failure.what();
        return owned(result);
    } catch (const Json::exception&) {
        return owned({{"ok", false}, {"errorCode", "invalid_input"}, {"error", "archive JSON metadata is invalid"}});
    } catch (...) {
        try { return owned({{"ok", false}, {"errorCode", "internal"}, {"error", "screen archive operation failed"}}); }
        catch (...) { return nullptr; }
    }
  } catch (...) { return nullptr; }
}
extern "C" void mai_screen_frame_archive_free(char* result) { std::free(result); }
