#include "MaiCloudVodAndroid.h"

#include "MaiCloudDriveClient.h"
#include "MaiCloudVod.h"

#include <jni.h>

#include <memory>
#include <string>
#include <vector>

namespace {

struct CancelPair {
    MaiCloudDriveCancel* account = mai_cloud_drive_cancel_create();
    MaiCloudVodCancel* vod = mai_cloud_vod_cancel_create();
    ~CancelPair() {
        mai_cloud_drive_cancel_destroy(account);
        mai_cloud_vod_cancel_destroy(vod);
    }
};

struct Source {
    JavaVM* vm = nullptr;
    jclass cloudDriveClass = nullptr;
    jmethodID proxyMethod = nullptr;
    std::string proxyRoute;
    MaiCloudVod* vod = nullptr;
};

int resolveProxy(const char* url, void* opaque, MaiDownloadProxy* output) {
    auto* source = static_cast<Source*>(opaque);
    JNIEnv* env = nullptr;
    bool attached = false;
    if (source->vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_EDETACHED) {
        if (source->vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return 0;
        attached = true;
    }
    int okay = 0;
    jstring address = env->NewStringUTF(url);
    if (address) {
        auto* selected = static_cast<jstring>(env->CallStaticObjectMethod(
            source->cloudDriveClass, source->proxyMethod, address));
        if (!env->ExceptionCheck() && selected) {
            const char* route = env->GetStringUTFChars(selected, nullptr);
            if (route) {
                source->proxyRoute = route;
                env->ReleaseStringUTFChars(selected, route);
                output->url_utf8 = source->proxyRoute.c_str();
                output->username_utf8 = nullptr;
                output->password_utf8 = nullptr;
                okay = 1;
            }
        }
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (selected) env->DeleteLocalRef(selected);
        env->DeleteLocalRef(address);
    }
    if (attached) source->vm->DetachCurrentThread();
    return okay;
}

void closeSource(JNIEnv* env, Source* source) {
    if (!source) return;
    if (source->vod) mai_cloud_vod_close(source->vod);
    if (source->cloudDriveClass) env->DeleteGlobalRef(source->cloudDriveClass);
    delete source;
}

} // namespace

int maiCloudVodAndroidReadAt(void* opaque, uint64_t offset,
                            uint8_t* output, int capacity) {
    auto* source = static_cast<Source*>(opaque);
    if (!source || !source->vod || !output || capacity <= 0) return -1;
    size_t bytes = 0;
    MaiCloudVodResult result{};
    return mai_cloud_vod_read_at(source->vod, offset, output,
                                static_cast<size_t>(capacity), &bytes, &result)
        ? static_cast<int>(bytes) : -1;
}

uint64_t maiCloudVodAndroidSize(void* opaque) {
    auto* source = static_cast<Source*>(opaque);
    return source && source->vod ? mai_cloud_vod_size(source->vod) : 0;
}

void maiCloudVodAndroidInterruptRead(void* opaque) {
    auto* source = static_cast<Source*>(opaque);
    if (source && source->vod) mai_cloud_vod_interrupt_read(source->vod);
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_kongshang_maichat_MaiCloudVod_nativeCreateCancel(JNIEnv*, jclass) {
    auto* cancel = new CancelPair;
    if (!cancel->account || !cancel->vod) { delete cancel; return 0; }
    return reinterpret_cast<jlong>(cancel);
}

extern "C" JNIEXPORT void JNICALL
Java_com_kongshang_maichat_MaiCloudVod_nativeRequestCancel(JNIEnv*, jclass, jlong handle) {
    auto* cancel = reinterpret_cast<CancelPair*>(handle);
    if (!cancel) return;
    mai_cloud_drive_cancel_request(cancel->account);
    mai_cloud_vod_cancel_request(cancel->vod);
}

extern "C" JNIEXPORT void JNICALL
Java_com_kongshang_maichat_MaiCloudVod_nativeDestroyCancel(JNIEnv*, jclass, jlong handle) {
    delete reinterpret_cast<CancelPair*>(handle);
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_kongshang_maichat_MaiCloudVod_nativeOpen(
    JNIEnv* env, jclass, jstring base, jstring token, jbyteArray pem,
    jstring suffix, jstring mediaId, jlong cancelHandle) {
    auto* cancel = reinterpret_cast<CancelPair*>(cancelHandle);
    if (!cancel || !base || !token || !pem || !mediaId) return 0;
    auto source = std::make_unique<Source>();
    if (env->GetJavaVM(&source->vm) != JNI_OK) return 0;
    jclass localClass = env->FindClass("com/kongshang/maichat/MaiCloudDrive");
    if (!localClass) return 0;
    source->cloudDriveClass = static_cast<jclass>(env->NewGlobalRef(localClass));
    source->proxyMethod = env->GetStaticMethodID(localClass, "proxyForUrl",
        "(Ljava/lang/String;)Ljava/lang/String;");
    env->DeleteLocalRef(localClass);
    if (!source->cloudDriveClass || !source->proxyMethod) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        closeSource(env, source.release());
        return 0;
    }
    const char* baseValue = env->GetStringUTFChars(base, nullptr);
    const char* tokenValue = env->GetStringUTFChars(token, nullptr);
    const char* suffixValue = suffix ? env->GetStringUTFChars(suffix, nullptr) : nullptr;
    const char* idValue = env->GetStringUTFChars(mediaId, nullptr);
    const jsize pemSize = env->GetArrayLength(pem);
    std::vector<char> roots(static_cast<size_t>(pemSize));
    if (pemSize > 0)
        env->GetByteArrayRegion(pem, 0, pemSize,
                                reinterpret_cast<jbyte*>(roots.data()));
    if (!baseValue || !tokenValue || !idValue || env->ExceptionCheck()) {
        if (baseValue) env->ReleaseStringUTFChars(base, baseValue);
        if (tokenValue) env->ReleaseStringUTFChars(token, tokenValue);
        if (suffixValue) env->ReleaseStringUTFChars(suffix, suffixValue);
        if (idValue) env->ReleaseStringUTFChars(mediaId, idValue);
        closeSource(env, source.release());
        return 0;
    }
    MaiCloudDriveOptions options{};
    options.auth_base_url_utf8 = baseValue;
    options.account_token_utf8 = tokenValue;
    options.ca_pem = roots.data();
    options.ca_pem_length = roots.size();
    options.signed_upload_host_suffix_utf8 = suffixValue;
    options.proxy_resolver = resolveProxy;
    options.proxy_context = source.get();
    MaiCloudVodResult result{};
    source->vod = mai_cloud_vod_open_cloud_drive(
        &options, idValue, cancel->account, cancel->vod, &result);
    env->ReleaseStringUTFChars(base, baseValue);
    env->ReleaseStringUTFChars(token, tokenValue);
    if (suffixValue) env->ReleaseStringUTFChars(suffix, suffixValue);
    env->ReleaseStringUTFChars(mediaId, idValue);
    if (!source->vod) {
        closeSource(env, source.release());
        return 0;
    }
    return reinterpret_cast<jlong>(source.release());
}

extern "C" JNIEXPORT void JNICALL
Java_com_kongshang_maichat_MaiCloudVod_nativeCancelSource(JNIEnv*, jclass, jlong handle) {
    auto* source = reinterpret_cast<Source*>(handle);
    if (source && source->vod) mai_cloud_vod_cancel(source->vod);
}

extern "C" JNIEXPORT void JNICALL
Java_com_kongshang_maichat_MaiCloudVod_nativeCloseSource(JNIEnv* env, jclass, jlong handle) {
    closeSource(env, reinterpret_cast<Source*>(handle));
}
