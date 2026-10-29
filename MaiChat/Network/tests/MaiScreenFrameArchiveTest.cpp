#include "MaiScreenFrameArchive.h"
#include <nlohmann/json.hpp>
#include <zlib.h>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
using Json = nlohmann::json;
#define CHECK(x) do { if (!(x)) throw std::runtime_error("failed: " #x); } while (0)
static const char* owner = "draft:\xe6\xb5\x8b\xe8\xaf\x95:session";
#ifdef _WIN32
static std::wstring wide(const std::string& text) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), int(text.size()), nullptr, 0);
    CHECK(n > 0); std::wstring result(n, 0);
    CHECK(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), int(text.size()), result.data(), n) == n);
    return result;
}
#endif
static FILE* openFile(const std::string& path, const char* mode) {
#ifdef _WIN32
    std::wstring m; while (*mode) m.push_back(*mode++);
    return _wfopen(wide(path).c_str(), m.c_str());
#else
    return std::fopen(path.c_str(), mode);
#endif
}
static void write(const std::string& path, const std::string& bytes) {
    FILE* file = openFile(path, "wb"); CHECK(file);
    CHECK(std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size()); CHECK(std::fclose(file) == 0);
}
static std::string read(const std::string& path) {
    FILE* file = openFile(path, "rb"); CHECK(file);
    CHECK(std::fseek(file, 0, SEEK_END) == 0); long n = std::ftell(file); CHECK(n >= 0);
    CHECK(std::fseek(file, 0, SEEK_SET) == 0); std::string result(size_t(n), 0);
    CHECK(std::fread(result.data(), 1, result.size(), file) == result.size()); std::fclose(file);
    return result;
}
static void remove(const std::string& path) {
#ifdef _WIN32
    CHECK(DeleteFileW(wide(path).c_str()));
#else
    CHECK(std::remove(path.c_str()) == 0);
#endif
}
static bool exists(const std::string& path) { FILE* f = openFile(path, "rb"); if (f) std::fclose(f); return f != nullptr; }
static void linkFile(const std::string& source, const std::string& destination) {
#ifdef _WIN32
    CHECK(CreateHardLinkW(wide(destination).c_str(), wide(source).c_str(), nullptr));
#else
    CHECK(link(source.c_str(), destination.c_str()) == 0);
#endif
}
static int64_t now() { return std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count(); }
static void u32(std::string& out, uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back(char(value >> shift));
}
static void chunk(std::string& png, const char* name, const std::string& data) {
    u32(png, uint32_t(data.size())); png.append(name, 4); png += data;
    uLong crc = crc32(0, reinterpret_cast<const Bytef*>(name), 4);
    crc = crc32(crc, reinterpret_cast<const Bytef*>(data.data()), uInt(data.size())); u32(png, uint32_t(crc));
}
static std::string png(unsigned width = 16, unsigned height = 16, bool large = false) {
    std::string result("\x89PNG\r\n\x1a\n", 8), header;
    u32(header, width); u32(header, height); header += std::string("\x08\x02\x00\x00\x00", 5);
    chunk(result, "IHDR", header);
    std::string pixels; pixels.reserve(size_t(width) * height * 3 + height);
    for (unsigned row = 0; row < height; ++row) {
        pixels.push_back(0);
        for (unsigned x = 0; x < width; ++x) pixels.append("\x1c\x8a\xd3", 3);
    }
    uLongf capacity = compressBound(uLong(pixels.size())); std::string compressed(capacity, 0);
    CHECK(compress2(reinterpret_cast<Bytef*>(compressed.data()), &capacity,
        reinterpret_cast<const Bytef*>(pixels.data()), uLong(pixels.size()), large ? Z_NO_COMPRESSION : Z_BEST_SPEED) == Z_OK);
    compressed.resize(capacity); chunk(result, "IDAT", compressed); chunk(result, "IEND", ""); return result;
}
static Json request(const std::string& root, Json input, const std::string* bytes = nullptr,
                    const char* session = owner) {
    char* response = mai_screen_frame_archive_request(root.c_str(), session, input.dump().c_str(),
        bytes ? bytes->data() : nullptr, bytes ? bytes->size() : 0);
    CHECK(response); Json result = Json::parse(response); mai_screen_frame_archive_free(response); return result;
}
static Json ok(const std::string& root, Json input, const std::string* bytes = nullptr) {
    const auto response = request(root, input, bytes);
    if (response["ok"] != true) throw std::runtime_error(response.dump());
    return response["output"];
}
static Json append(const std::string& batch, int64_t elapsed, unsigned w = 16, unsigned h = 16) {
    return {{"action", "append"}, {"batch_id", batch}, {"width", w}, {"height", h},
        {"captured_at_ms", now()}, {"elapsed_ms", elapsed}};
}
static std::string begin(const std::string& root) { return ok(root, {{"action", "begin"}})["batch_id"]; }
static void seed(const std::string& root, const Json& frame, unsigned count) {
    const std::string relative = frame["path"];
    const auto slash = relative.find_last_of('/');
    const std::string folder = root + "/" + relative.substr(0, slash);
    const std::string file = folder + "/manifest.json";
    Json manifest = Json::parse(read(file));
    Json first = manifest["frames"][0]; manifest["frames"] = Json::array();
    const int64_t start = now() - int64_t(count) * 2000 - 4000;
    manifest["started_at_ms"] = start;
    for (unsigned i = 0; i < count; ++i) {
        Json next = first;
        if (i) {
            char id[48]; std::snprintf(id, sizeof(id), "frame_%032x", i);
            next["id"] = id;
            next["path"] = relative.substr(0, slash + 1) + id + ".png";
            linkFile(root + "/" + relative, root + "/" + next["path"].get<std::string>());
        }
        next["captured_at_ms"] = start + int64_t(i) * 2000;
        next["saved_at_ms"] = start + int64_t(i) * 2000;
        next["elapsed_ms"] = int64_t(i) * 2000;
        manifest["frames"].push_back(next);
    }
    manifest["total_bytes"] = uint64_t(first["size_bytes"]) * count;
    write(file, manifest.dump());
}
static void run(const std::string& root) {
    const std::string tiny = png();
    CHECK(request(root, {{"action", "begin"}}, nullptr, "")["errorCode"] == "invalid_input");
    CHECK(request(root, {{"action", "begin"}, {"path", "../outside"}})["errorCode"] == "invalid_input");
    CHECK(request(root, {{"action", "get"}, {"batch_id", "../outside"}, {"frame_id", "bad"}})["ok"] == false);
    const std::string batch = begin(root);
    CHECK(request(root, {{"action", "list"}}, nullptr, "remote:other:session")["output"]["batches"].empty());
    CHECK(request(root, {{"action", "list"}, {"batch_id", batch}}, nullptr, "remote:other:session")["errorCode"] == "not_found");
    const auto first = ok(root, append(batch, 0), &tiny)["frame"];
    CHECK(first["width"] == 16 && first["height"] == 16 && first["size_bytes"] == tiny.size());
    const std::string relative = first["path"];
    CHECK(relative.rfind(".maichat/screen-frame-archive/", 0) == 0 && relative.find("..") == std::string::npos);
    const auto get = ok(root, {{"action", "get"}, {"batch_id", batch}, {"frame_id", first["id"]}});
    CHECK(get["path"] == first["path"] && read(root + "/" + relative) == tiny);
    CHECK(request(root, append(batch, 2000), &tiny)["errorCode"] == "rate_limited");
    const std::string malformedBatch = begin(root);
    CHECK(request(root, append(malformedBatch, 0, 15, 16), &tiny)["errorCode"] == "invalid_input");
    std::string broken = tiny; broken.back() ^= 1;
    CHECK(request(root, append(malformedBatch, 0), &broken)["errorCode"] == "invalid_input");
    CHECK(ok(root, {{"action", "list"}, {"batch_id", malformedBatch}})["frames"].empty());
    ok(root, {{"action", "discard"}, {"batch_id", malformedBatch}});
    const std::string concurrent = begin(root);
    std::array<Json, 2> replies;
    std::thread a([&] { replies[0] = request(root, append(concurrent, 0), &tiny); });
    std::thread b([&] { replies[1] = request(root, append(concurrent, 0), &tiny); });
    a.join(); b.join();
    CHECK(int(replies[0]["ok"] == true) + int(replies[1]["ok"] == true) == 1);
    CHECK(ok(root, {{"action", "list"}, {"batch_id", concurrent}})["frames"].size() == 1);
    ok(root, {{"action", "discard"}, {"batch_id", concurrent}});
    std::this_thread::sleep_for(std::chrono::milliseconds(2100));
    CHECK(ok(root, append(batch, 2000), &tiny)["batch"]["frame_count"] == 2);
    write(root + "/" + relative, broken);
    CHECK(request(root, {{"action", "get"}, {"batch_id", batch}, {"frame_id", first["id"]}})["errorCode"] == "invalid_input");
    write(root + "/" + relative, tiny);
#if !defined(_WIN32)
    const std::string ownerPath = root + "/" + relative.substr(0, relative.find("/batch_"));
    const std::string savedOwner = ownerPath + ".saved";
    CHECK(std::rename(ownerPath.c_str(), savedOwner.c_str()) == 0);
    CHECK(symlink(savedOwner.c_str(), ownerPath.c_str()) == 0);
    CHECK(request(root, {{"action", "get"}, {"batch_id", batch},
        {"frame_id", first["id"]}})["errorCode"] == "invalid_input");
    CHECK(unlink(ownerPath.c_str()) == 0);
    CHECK(std::rename(savedOwner.c_str(), ownerPath.c_str()) == 0);
#endif
    ok(root, {{"action", "finish"}, {"batch_id", batch}, {"reason", "system_stopped"}});
    CHECK(ok(root, {{"action", "finish"}, {"batch_id", batch}, {"reason", "again"}})["batch"]["finish_reason"] == "system_stopped");
    CHECK(request(root, append(batch, 4000), &tiny)["errorCode"] == "invalid_input");
    // A recording batch surviving process death must not prevent a fresh begin.
    const std::string stale = begin(root); const std::string fresh = begin(root);
    ok(root, {{"action", "finish"}, {"batch_id", stale}, {"reason", "interrupted"}});
    ok(root, {{"action", "discard"}, {"batch_id", fresh}});
    ok(root, {{"action", "discard"}, {"batch_id", stale}});
    const std::string full = begin(root);
    const auto fullFrame = ok(root, append(full, 0), &tiny)["frame"];
    seed(root, fullFrame, 300);
    const auto countLimit = request(root, append(full, 600000), &tiny);
    CHECK(countLimit["errorCode"] == "limit" && countLimit["terminal"] == true && countLimit["limit_reason"] == "batch_frames");
    CHECK(ok(root, {{"action", "list"}, {"batch_id", full}})["frames"].size() == 300);
    ok(root, {{"action", "discard"}, {"batch_id", full}});
    // Valid, uncompressed PNGs plus hardlinks create a reopened 240 MiB batch
    // without retaining duplicate physical test data on disk.
    const std::string large = png(2600, 2500, true);
    CHECK(large.size() > 19'000'000 && large.size() <= 20'000'000);
    const std::string bytesBatch = begin(root);
    const auto largeFrame = ok(root, append(bytesBatch, 0, 2600, 2500), &large)["frame"];
    seed(root, largeFrame, 13);
    const auto byteLimit = request(root, append(bytesBatch, 26000, 2600, 2500), &large);
    CHECK(byteLimit["errorCode"] == "limit" && byteLimit["limit_reason"] == "batch_bytes");
    ok(root, {{"action", "discard"}, {"batch_id", bytesBatch}});
    const std::string quota = root + "/.maichat/screen-frame-archive/quota.bin";
    FILE* sparse = openFile(quota, "wb"); CHECK(sparse);
#ifdef _WIN32
    CHECK(_chsize_s(_fileno(sparse), 512ULL * 1024 * 1024) == 0);
#else
    CHECK(ftruncate(fileno(sparse), 512ULL * 1024 * 1024) == 0);
#endif
    std::fclose(sparse);
    const auto workspaceLimit = request(root, {{"action", "begin"}});
    CHECK(workspaceLimit["errorCode"] == "limit" && workspaceLimit["limit_reason"] == "workspace_bytes");
    remove(quota);
    write(root + "/test-process-race.json", Json{{"batch", begin(root)}}.dump());
    // Persist a receipt so a separate process can reopen and explicitly discard.
    write(root + "/test-reopen.json", Json{{"batch", batch}, {"frame", first}}.dump());
    std::puts("PASS: archive PNG validation, owner scope, intervals, concurrency and all storage limits");
}
static void reopen(const std::string& root) {
    const auto race = Json::parse(read(root + "/test-process-race.json"));
    CHECK(ok(root, {{"action", "list"}, {"batch_id", race["batch"]}})["frames"].size() == 1);
    ok(root, {{"action", "discard"}, {"batch_id", race["batch"]}});
    const auto state = Json::parse(read(root + "/test-reopen.json"));
    const std::string batch = state["batch"];
    const auto listing = ok(root, {{"action", "list"}, {"batch_id", batch}});
    CHECK(listing["batches"][0]["status"] == "stopped" && listing["frames"].size() == 2);
    const auto frame = ok(root, {{"action", "get"}, {"batch_id", batch}, {"frame_id", state["frame"]["id"]}});
    const std::string selected = root + "/selected-copy.png";
    write(selected, read(root + "/" + frame["path"].get<std::string>()));
    CHECK(ok(root, {{"action", "discard"}, {"batch_id", batch}})["removed_frames"] == 2);
    CHECK(exists(selected));
    CHECK(!exists(root + "/" + frame["path"].get<std::string>()));
    CHECK(ok(root, {{"action", "list"}})["frames"].empty());
    std::puts("PASS: fresh process reopen, stopped-frame retention and selected copy survives discard");
}
static int entry(int argc, const std::vector<std::string>& args) {
    try { CHECK(argc == 3); if (args[1] == "--run") run(args[2]);
          else if (args[1] == "--process-race") {
              const auto state = Json::parse(read(args[2] + "/test-process-race.json"));
              const auto bytes = png();
              const auto result = request(args[2], append(state["batch"], 0), &bytes);
              CHECK(result["ok"] == true || result["errorCode"] == "rate_limited");
              std::puts(result["ok"] == true ? "race.accepted" : "race.rate_limited");
          }
          else if (args[1] == "--reopen") reopen(args[2]); else CHECK(false); return 0; }
    catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
}
#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i) { int size = WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, nullptr, 0, nullptr, nullptr);
        std::string value(size_t(size), 0); WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, value.data(), size, nullptr, nullptr); value.pop_back(); args.push_back(value); }
    return entry(argc, args);
}
#else
int main(int argc, char** argv) { return entry(argc, std::vector<std::string>(argv, argv + argc)); }
#endif
