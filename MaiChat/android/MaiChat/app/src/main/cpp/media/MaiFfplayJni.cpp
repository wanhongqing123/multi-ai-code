#include <jni.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include "MaiFfplayEntry.h"
#include "MaiGraphicsPresenter.h"

namespace {

struct PlaybackSession {
    uint64_t viewId = 0;
    std::string path;
    MaiFfplayHost host{};
    std::thread worker;
    std::atomic<bool> finished{false};
    std::atomic<int> result{0};
};

bool presentVideo(void* user, const MaiVideoFrame* frame,
                  const MaiVideoSubtitle* subtitle) {
    auto* session = static_cast<PlaybackSession*>(user);
    return maiGraphicsPresenterShowVideoFrameWithSubtitle(session->viewId,
                                                          frame, subtitle, false);
}

bool presentRgba(void* user, const uint8_t* pixels,
                 uint32_t width, uint32_t height, uint32_t stride) {
    auto* session = static_cast<PlaybackSession*>(user);
    return maiGraphicsPresenterShowFrame(session->viewId, pixels, width,
                                         height, stride, false);
}

}  // namespace

extern "C" JNIEXPORT jlong JNICALL
Java_com_kongshang_maichat_MaiFfplayVideoView_nativeCreate(
    JNIEnv* env, jclass, jlong viewId, jstring path) {
    if (viewId <= 0 || !path) return 0;
    const char* value = env->GetStringUTFChars(path, nullptr);
    if (!value) return 0;
    auto* session = new PlaybackSession;
    session->viewId = static_cast<uint64_t>(viewId);
    session->path = value;
    env->ReleaseStringUTFChars(path, value);
    if (session->path.empty()) {
        delete session;
        return 0;
    }
    session->host.user_data = session;
    session->host.graphics_view_id = session->viewId;
    session->host.present_video = presentVideo;
    session->host.present_rgba = presentRgba;
    try {
        session->worker = std::thread([session] {
            char name[] = "ffplay";
            std::string input = session->path;
            char* arguments[] = {name, &input[0]};
            session->result = maiFfplayRun(&session->host, 2, arguments);
            session->finished = true;
        });
    } catch (...) {
        delete session;
        return 0;
    }
    return reinterpret_cast<jlong>(session);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_kongshang_maichat_MaiFfplayVideoView_nativeCommand(
    JNIEnv* env, jclass, jlong handle, jstring command) {
    if (!handle || !command) return JNI_FALSE;
    const char* value = env->GetStringUTFChars(command, nullptr);
    if (!value) return JNI_FALSE;
    const int accepted = maiFfplaySendCommand(value);
    env->ReleaseStringUTFChars(command, value);
    return accepted == 1 ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_kongshang_maichat_MaiFfplayVideoView_nativeSeekPercent(
    JNIEnv*, jclass, jlong handle, jdouble fraction) {
    return handle && maiFfplaySeekPercent(fraction) == 1 ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jlongArray JNICALL
Java_com_kongshang_maichat_MaiFfplayVideoView_nativePlaybackStatus(
    JNIEnv* env, jclass, jlong handle) {
    if (!handle) return nullptr;
    MaiFfplayPlaybackStatus status{};
    if (!maiFfplayGetPlaybackStatus(&status)) return nullptr;
    const jlong values[] = {status.position_us, status.duration_us,
                            status.running, status.paused};
    jlongArray result = env->NewLongArray(4);
    if (result) env->SetLongArrayRegion(result, 0, 4, values);
    return result;
}

extern "C" JNIEXPORT void JNICALL
Java_com_kongshang_maichat_MaiFfplayVideoView_nativeDestroy(
    JNIEnv*, jclass, jlong handle) {
    auto* session = reinterpret_cast<PlaybackSession*>(handle);
    if (!session) return;
    for (int attempt = 0; attempt < 100 && !session->finished; ++attempt) {
        if (maiFfplaySendCommand("close") == 1) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (session->worker.joinable()) session->worker.join();
    delete session;
}
