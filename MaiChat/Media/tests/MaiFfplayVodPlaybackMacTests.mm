#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>

#include <curl/curl.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <thread>

#include "MaiCloudVod.h"
#include "MaiFfplayEntry.h"
#include "MaiGraphicsPresenter.h"

namespace {

struct Session {
    uint64_t viewId = 0;
    MaiCloudVod* source = nullptr;
    std::atomic<unsigned> frames{0};
    std::atomic<unsigned> drawn{0};
    std::atomic<unsigned> reads{0};
};

void onPresent(uint64_t, bool success, void* user) {
    if (success) ++static_cast<Session*>(user)->drawn;
}

struct Metrics {
    unsigned ranges = 0;
    unsigned long long bytes = 0;
    unsigned auth = 0;
    unsigned long long highestRange = 0;
};

size_t collect(char* bytes, size_t size, size_t count, void* context) {
    const size_t length = size * count;
    static_cast<std::string*>(context)->append(bytes, length);
    return length;
}

bool readMetrics(const std::string& base, const char* caPath, Metrics* metrics) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;
    const std::string url = base + "/metrics";
    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_CAINFO, caPath);
    curl_easy_setopt(curl, CURLOPT_PROXY, "");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, collect);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    const CURLcode code = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    return code == CURLE_OK &&
        std::sscanf(response.c_str(), "%u %llu %u %llu", &metrics->ranges,
                    &metrics->bytes, &metrics->auth,
                    &metrics->highestRange) == 4;
}

int readAt(void* user, uint64_t offset, uint8_t* output, int capacity) {
    auto* session = static_cast<Session*>(user);
    size_t bytes = 0;
    MaiCloudVodResult result{};
    if (!mai_cloud_vod_read_at(session->source, offset, output,
                               size_t(capacity), &bytes, &result)) return -1;
    ++session->reads;
    return int(bytes);
}

bool presentVideo(void* user, const MaiVideoFrame* frame,
                  const MaiVideoSubtitle* subtitle) {
    auto* session = static_cast<Session*>(user);
    const bool shown = maiGraphicsPresenterShowVideoFrameWithSubtitle(
        session->viewId, frame, subtitle, false);
    if (shown) ++session->frames;
    return shown;
}

bool presentRgba(void* user, const uint8_t* pixels,
                 uint32_t width, uint32_t height, uint32_t stride) {
    auto* session = static_cast<Session*>(user);
    const bool shown = maiGraphicsPresenterShowFrame(session->viewId, pixels,
                                                      width, height, stride, false);
    if (shown) ++session->frames;
    return shown;
}

bool waitUntil(const std::atomic<bool>& finished,
               const std::function<bool()>& predicate, double seconds) {
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::duration<double>(seconds);
    while (!predicate() && !finished.load() &&
           std::chrono::steady_clock::now() < deadline) {
        [[NSRunLoop currentRunLoop]
            runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.02]];
    }
    return predicate();
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 6) return 2;
    @autoreleasepool {
        [NSApplication sharedApplication];
        NSWindow* window = [[NSWindow alloc]
            initWithContentRect:NSMakeRect(0, 0, 320, 180)
                      styleMask:NSWindowStyleMaskTitled
                        backing:NSBackingStoreBuffered defer:NO];
        NSView* view = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 320, 180)];
        CAMetalLayer* layer = [CAMetalLayer layer];
        layer.frame = view.bounds;
        view.wantsLayer = YES;
        view.layer = layer;
        window.contentView = view;
        [window makeKeyAndOrderFront:nil];
        Session session;
        if (!maiGraphicsPresenterStart(argv[1], argv[2], onPresent, &session)) return 3;
        session.viewId = maiGraphicsPresenterAttach((__bridge void*)layer,
                                                    320, 180, nullptr, nullptr);
        if (!session.viewId) return 4;

        const std::string base = argv[3];
        const std::string url = base + "/oss/play?sig=valid";
        const uint64_t fileSize = std::strtoull(argv[5], nullptr, 10);
        MaiCloudVodOptions options{};
        options.signed_url_utf8 = url.c_str();
        options.signed_host_suffix_utf8 = "localhost";
        options.expected_size_bytes = fileSize;
        options.ca_path_utf8 = argv[4];
        options.proxy_url_utf8 = "";
        MaiCloudVodResult openResult{};
        session.source = mai_cloud_vod_open(&options, &openResult);
        if (!session.source || openResult.status != MAI_CLOUD_VOD_OK) return 5;

        MaiFfplayHost host{};
        host.user_data = &session;
        host.graphics_view_id = session.viewId;
        host.present_video = presentVideo;
        host.present_rgba = presentRgba;
        host.read_input_at = readAt;
        host.input_size_bytes = mai_cloud_vod_size(session.source);
        std::atomic<bool> finished{false};
        int exitCode = -1;
        char name[] = "ffplay";
        char noStats[] = "-nostats";
        char loop[] = "-loop";
        char noLoop[] = "0";
        char path[] = "maicloud-vod.mov";
        char* arguments[] = {name, noStats, loop, noLoop, path};
        std::thread playback([&] {
            exitCode = maiFfplayRunAtSpeed(&host, 5, arguments, 1.0, 0);
            finished = true;
        });
        const bool firstFrame = waitUntil(finished,
            [&] { return session.frames.load() > 0 &&
                         session.drawn.load() > 0; }, 12);
        Metrics firstMetrics{};
        const bool measuredFirst = firstFrame &&
            readMetrics(base, argv[4], &firstMetrics);
        const bool streamedEarly = measuredFirst && firstMetrics.ranges >= 2 &&
            firstMetrics.bytes < fileSize &&
            firstMetrics.highestRange >= fileSize - 1024 * 1024;
        const bool paused = firstFrame && maiFfplaySendCommand("pause") == 1;
        [[NSRunLoop currentRunLoop]
            runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.25]];
        Metrics beforeSeek{};
        const bool measuredBeforeSeek = paused &&
            readMetrics(base, argv[4], &beforeSeek);
        const unsigned framesBeforeSeek = session.frames.load();
        const unsigned drawnBeforeSeek = session.drawn.load();
        const bool seekAccepted = measuredBeforeSeek &&
            maiFfplaySeekAbsoluteUs(11'000'000) == 1;
        const bool rangeAfterSeek = seekAccepted && waitUntil(finished, [&] {
            Metrics current{};
            return readMetrics(base, argv[4], &current) &&
                current.ranges > beforeSeek.ranges;
        }, 8);
        const bool resumed = rangeAfterSeek &&
            maiFfplaySendCommand("pause") == 1;
        MaiFfplayPlaybackStatus status{};
        const bool laterFrame = resumed && waitUntil(finished, [&] {
            return session.frames.load() > framesBeforeSeek &&
                session.drawn.load() > drawnBeforeSeek &&
                maiFfplayGetPlaybackStatus(&status) &&
                status.position_us >= 10'000'000;
        }, 8);
        Metrics afterSeek{};
        const bool measuredAfterSeek = laterFrame &&
            readMetrics(base, argv[4], &afterSeek);
        const bool cacheBounded =
            mai_cloud_vod_cached_bytes(session.source) <= 8 * 1024 * 1024;
        mai_cloud_vod_cancel(session.source);
        maiFfplayRequestQuit();
        const bool closed = waitUntil(finished,
            [&] { return finished.load(); }, 6);
        if (!closed) std::quick_exit(6);
        playback.join();
        mai_cloud_vod_close(session.source);
        maiGraphicsPresenterDetach(session.viewId);
        maiGraphicsPresenterStop();
        std::fprintf(stderr,
                     "VOD first=%d early=%d seek-range=%d later-frame=%d "
                     "closed=%d reads=%u player-exit=%d "
                     "first-bytes=%llu/%llu first-ranges=%u "
                     "seek-ranges=%u->%u cache<=8MiB=%d\n",
                     firstFrame, streamedEarly, rangeAfterSeek, laterFrame,
                     closed, session.reads.load(), exitCode,
                     firstMetrics.bytes, static_cast<unsigned long long>(fileSize),
                     firstMetrics.ranges, beforeSeek.ranges, afterSeek.ranges,
                     cacheBounded);
        return firstFrame && streamedEarly && rangeAfterSeek && laterFrame &&
            measuredAfterSeek && cacheBounded && closed ? 0 : 7;
    }
}
