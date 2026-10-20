// Bridges Android lifecycle and UTF-8 frames to the shared C++ WebSocket client.
#include "MaiSessionSyncC.h"

#include <atomic>
#include <jni.h>
#include <new>
#include <string>

namespace {

struct AndroidSocket {
    JavaVM* vm = nullptr;
    jobject target = nullptr;
    jmethodID callback = nullptr;
    MaiSessionSyncHandle* transport = nullptr;
    std::atomic<bool> closing{false};
};

void receiveFrame(void* context, int event, const char* bytes, size_t length) {
    auto* socket = static_cast<AndroidSocket*>(context);
    if (socket->closing.load(std::memory_order_acquire)) return;
    JNIEnv* environment = nullptr;
    bool detach = false;
    if (socket->vm->GetEnv(reinterpret_cast<void**>(&environment), JNI_VERSION_1_6) == JNI_EDETACHED) {
        if (socket->vm->AttachCurrentThread(&environment, nullptr) != JNI_OK) return;
        detach = true;
    }
    if (environment != nullptr && length <= 1024 * 1024) {
        auto* data = environment->NewByteArray(static_cast<jsize>(length));
        if (data != nullptr) {
            if (length)
                environment->SetByteArrayRegion(data, 0, static_cast<jsize>(length),
                                                reinterpret_cast<const jbyte*>(bytes));
            if (!environment->ExceptionCheck())
                environment->CallVoidMethod(socket->target, socket->callback, static_cast<jint>(event), data);
            environment->DeleteLocalRef(data);
        }
        if (environment->ExceptionCheck()) environment->ExceptionClear();
    }
    if (detach) socket->vm->DetachCurrentThread();
}

std::string copyString(JNIEnv* environment, jstring value) {
    if (value == nullptr) return {};
    const char* source = environment->GetStringUTFChars(value, nullptr);
    if (source == nullptr) return {};
    std::string result(source);
    environment->ReleaseStringUTFChars(value, source);
    return result;
}

AndroidSocket* socketFrom(jlong handle) {
    return reinterpret_cast<AndroidSocket*>(handle);
}

}  // namespace

extern "C" JNIEXPORT jlong JNICALL
Java_com_kongshang_maichat_AndroidSessionSyncCoordinator_nativeCreate(JNIEnv* environment, jobject owner) {
    auto* socket = new (std::nothrow) AndroidSocket();
    if (socket == nullptr) return 0;
    if (environment->GetJavaVM(&socket->vm) != JNI_OK) {
        delete socket;
        return 0;
    }
    socket->target = environment->NewGlobalRef(owner);
    jclass ownerClass = environment->GetObjectClass(owner);
    socket->callback =
        ownerClass == nullptr ? nullptr : environment->GetMethodID(ownerClass, "onNativeSocketEvent", "(I[B)V");
    if (ownerClass != nullptr) environment->DeleteLocalRef(ownerClass);
    if (socket->target == nullptr || socket->callback == nullptr || environment->ExceptionCheck()) {
        if (environment->ExceptionCheck()) environment->ExceptionClear();
        if (socket->target != nullptr) environment->DeleteGlobalRef(socket->target);
        delete socket;
        return 0;
    }
    socket->transport = maiSessionSyncCreate(receiveFrame, socket);
    if (socket->transport == nullptr) {
        environment->DeleteGlobalRef(socket->target);
        delete socket;
        return 0;
    }
    return reinterpret_cast<jlong>(socket);
}

extern "C" JNIEXPORT jboolean JNICALL Java_com_kongshang_maichat_AndroidSessionSyncCoordinator_nativeStart(
    JNIEnv* environment, jobject, jlong handle, jstring url, jstring bearer, jstring caPem, jstring proxy) {
    auto* socket = socketFrom(handle);
    if (socket == nullptr || socket->closing.load(std::memory_order_acquire)) return JNI_FALSE;
    const std::string endpoint = copyString(environment, url);
    const std::string token = copyString(environment, bearer);
    const std::string certificate = copyString(environment, caPem);
    const std::string proxyUrl = copyString(environment, proxy);
    return maiSessionSyncStart(socket->transport, endpoint.c_str(), token.c_str(), certificate.c_str(),
                               proxyUrl.c_str())
               ? JNI_TRUE
               : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL Java_com_kongshang_maichat_AndroidSessionSyncCoordinator_nativeSend(
    JNIEnv* environment, jobject, jlong handle, jbyteArray frame) {
    auto* socket = socketFrom(handle);
    if (socket == nullptr || frame == nullptr || socket->closing.load(std::memory_order_acquire)) return JNI_FALSE;
    const jsize length = environment->GetArrayLength(frame);
    if (length <= 0 || length > 96 * 1024) return JNI_FALSE;
    std::string bytes(static_cast<size_t>(length), '\0');
    environment->GetByteArrayRegion(frame, 0, length, reinterpret_cast<jbyte*>(bytes.data()));
    if (environment->ExceptionCheck()) {
        environment->ExceptionClear();
        return JNI_FALSE;
    }
    return maiSessionSyncSend(socket->transport, bytes.data(), bytes.size()) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL Java_com_kongshang_maichat_AndroidSessionSyncCoordinator_nativeUpdateBearer(
    JNIEnv* environment, jobject, jlong handle, jstring bearer) {
    auto* socket = socketFrom(handle);
    if (socket == nullptr || socket->closing.load(std::memory_order_acquire)) return JNI_FALSE;
    const std::string token = copyString(environment, bearer);
    return maiSessionSyncUpdateBearer(socket->transport, token.c_str()) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_kongshang_maichat_AndroidSessionSyncCoordinator_nativeDestroy(JNIEnv* environment, jobject, jlong handle) {
    auto* socket = socketFrom(handle);
    if (socket == nullptr) return;
    socket->closing.store(true, std::memory_order_release);
    maiSessionSyncDestroy(socket->transport);
    environment->DeleteGlobalRef(socket->target);
    delete socket;
}
