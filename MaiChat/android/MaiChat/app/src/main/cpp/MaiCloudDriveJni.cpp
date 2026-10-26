#include "MaiCloudDriveClient.h"

#include <jni.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace {

bool readString(JNIEnv* env, jstring input, std::string* output) {
    if (!input) return false;
    const jsize length = env->GetStringLength(input);
    const jchar* data = env->GetStringChars(input, nullptr);
    if (!data) return false;
    output->clear();
    output->reserve(static_cast<size_t>(length) * 3);
    for (jsize index = 0; index < length; ++index) {
        uint32_t character = data[index];
        if (character >= 0xD800 && character <= 0xDBFF && index + 1 < length &&
            data[index + 1] >= 0xDC00 && data[index + 1] <= 0xDFFF) {
            character = 0x10000 + ((character - 0xD800) << 10) +
                        (data[++index] - 0xDC00);
        } else if (character >= 0xD800 && character <= 0xDFFF) {
            env->ReleaseStringChars(input, data);
            return false;
        }
        if (character < 0x80) output->push_back(static_cast<char>(character));
        else if (character < 0x800) {
            output->push_back(static_cast<char>(0xC0 | (character >> 6)));
            output->push_back(static_cast<char>(0x80 | (character & 0x3F)));
        } else if (character < 0x10000) {
            output->push_back(static_cast<char>(0xE0 | (character >> 12)));
            output->push_back(static_cast<char>(0x80 | ((character >> 6) & 0x3F)));
            output->push_back(static_cast<char>(0x80 | (character & 0x3F)));
        } else {
            output->push_back(static_cast<char>(0xF0 | (character >> 18)));
            output->push_back(static_cast<char>(0x80 | ((character >> 12) & 0x3F)));
            output->push_back(static_cast<char>(0x80 | ((character >> 6) & 0x3F)));
            output->push_back(static_cast<char>(0x80 | (character & 0x3F)));
        }
    }
    env->ReleaseStringChars(input, data);
    return true;
}

bool readPem(JNIEnv* env, jbyteArray input, std::vector<char>* output) {
    if (!input) return false;
    const jsize length = env->GetArrayLength(input);
    if (length < 0 || length > 2 * 1024 * 1024) return false;
    output->resize(static_cast<size_t>(length));
    if (length > 0) env->GetByteArrayRegion(input, 0, length,
        reinterpret_cast<jbyte*>(output->data()));
    return !env->ExceptionCheck();
}

jobject makeResult(JNIEnv* env, const MaiCloudDriveResult& result) {
    jclass type = env->FindClass("com/kongshang/maichat/MaiCloudDrive$Result");
    if (!type) return nullptr;
    const jmethodID constructor = env->GetMethodID(type, "<init>", "(IJI[B)V");
    if (!constructor) { env->DeleteLocalRef(type); return nullptr; }
    if (result.response_bytes > static_cast<size_t>(std::numeric_limits<jsize>::max())) {
        env->DeleteLocalRef(type);
        return nullptr;
    }
    jbyteArray bytes = env->NewByteArray(static_cast<jsize>(result.response_bytes));
    if (!bytes) { env->DeleteLocalRef(type); return nullptr; }
    if (result.response_bytes > 0) env->SetByteArrayRegion(bytes, 0,
        static_cast<jsize>(result.response_bytes),
        reinterpret_cast<const jbyte*>(result.response_json_utf8));
    jobject output = env->ExceptionCheck() ? nullptr :
        env->NewObject(type, constructor, jint(result.status), jlong(result.http_status),
                       jint(result.curl_code), bytes);
    env->DeleteLocalRef(bytes);
    env->DeleteLocalRef(type);
    return output;
}

struct ProxyContext {
    JNIEnv* env = nullptr;
    jclass type = nullptr;
    jmethodID method = nullptr;
    std::string route;
};

int resolveProxy(const char* url, void* opaque, MaiDownloadProxy* output) {
    auto* context = static_cast<ProxyContext*>(opaque);
    jstring address = context->env->NewStringUTF(url);
    if (!address) return 0;
    auto* selected = static_cast<jstring>(context->env->CallStaticObjectMethod(
        context->type, context->method, address));
    context->env->DeleteLocalRef(address);
    if (context->env->ExceptionCheck() || !selected) return 0;
    const bool okay = readString(context->env, selected, &context->route);
    context->env->DeleteLocalRef(selected);
    if (!okay) return 0;
    output->url_utf8 = context->route.c_str();
    output->username_utf8 = nullptr;
    output->password_utf8 = nullptr;
    return 1;
}

bool configureProxy(JNIEnv* env, ProxyContext* context) {
    context->env = env;
    context->type = env->FindClass("com/kongshang/maichat/MaiCloudDrive");
    if (!context->type) return false;
    context->method = env->GetStaticMethodID(context->type, "proxyForUrl",
        "(Ljava/lang/String;)Ljava/lang/String;");
    if (context->method) return true;
    env->DeleteLocalRef(context->type);
    context->type = nullptr;
    return false;
}

struct Call {
    std::string base, token, payload, suffix, path, checkpoint, name, type, stableId;
    std::vector<char> pem;
    ProxyContext proxy;
    MaiCloudDriveOptions options{};
};

bool readCall(JNIEnv* env, jstring base, jstring token, jbyteArray pem,
              jstring suffix, Call* call) {
    if (!readString(env, base, &call->base) ||
        !readString(env, token, &call->token) ||
        !readPem(env, pem, &call->pem) ||
        (suffix && !readString(env, suffix, &call->suffix)) ||
        !configureProxy(env, &call->proxy)) return false;
    call->options.auth_base_url_utf8 = call->base.c_str();
    call->options.account_token_utf8 = call->token.c_str();
    call->options.ca_pem = call->pem.empty() ? nullptr : call->pem.data();
    call->options.ca_pem_length = call->pem.size();
    call->options.signed_upload_host_suffix_utf8 = suffix ? call->suffix.c_str() : nullptr;
    call->options.proxy_resolver = resolveProxy;
    call->options.proxy_context = &call->proxy;
    return true;
}

struct ProgressContext {
    JNIEnv* env = nullptr;
    jobject callback = nullptr;
    jmethodID method = nullptr;
};

bool configureProgress(JNIEnv* env, jobject callback, ProgressContext* context) {
    if (!callback) return false;
    jclass type = env->GetObjectClass(callback);
    if (!type) return false;
    context->method = env->GetMethodID(type, "update", "(JJ)I");
    env->DeleteLocalRef(type);
    if (!context->method) return false;
    context->env = env;
    context->callback = env->NewGlobalRef(callback);
    return context->callback != nullptr;
}

void releaseProgress(ProgressContext* context) {
    if (context->callback) context->env->DeleteGlobalRef(context->callback);
    context->callback = nullptr;
}

int progress(uint64_t done, uint64_t total, void* opaque) {
    auto* context = static_cast<ProgressContext*>(opaque);
    const jint canceled = context->env->CallIntMethod(context->callback, context->method,
        jlong(done), jlong(total));
    if (context->env->ExceptionCheck()) {
        context->env->ExceptionClear();
        return 1;
    }
    return canceled;
}

struct FdContext { int fd; };

int readAt(void* opaque, uint64_t offset, unsigned char* output, size_t count) {
    auto* source = static_cast<FdContext*>(opaque);
    if (offset > static_cast<uint64_t>(std::numeric_limits<off_t>::max())) return 0;
    size_t filled = 0;
    while (filled < count) {
        ssize_t bytes = pread(source->fd, output + filled, count - filled,
                              static_cast<off_t>(offset + filled));
        if (bytes < 0 && errno == EINTR) continue;
        if (bytes < 0 && errno == ESPIPE) return -1;
        if (bytes <= 0) return 0;
        filled += static_cast<size_t>(bytes);
    }
    return 1;
}

int readNext(void* opaque, unsigned char* output, size_t capacity, size_t* received) {
    auto* source = static_cast<FdContext*>(opaque);
    for (;;) {
        const ssize_t bytes = read(source->fd, output, capacity);
        if (bytes < 0 && errno == EINTR) continue;
        if (bytes < 0) return 0;
        *received = static_cast<size_t>(bytes);
        return 1;
    }
}

jobject complete(JNIEnv* env, Call* call, MaiCloudDriveResult* result) {
    if (call->proxy.type) env->DeleteLocalRef(call->proxy.type);
    if (env->ExceptionCheck()) {
        mai_cloud_drive_result_free(result);
        return nullptr;
    }
    jobject output = makeResult(env, *result);
    mai_cloud_drive_result_free(result);
    return output;
}

}  // namespace

extern "C" JNIEXPORT jlong JNICALL
Java_com_kongshang_maichat_MaiCloudDrive_nativeCreateCancel(JNIEnv*, jclass) {
    return reinterpret_cast<jlong>(mai_cloud_drive_cancel_create());
}

extern "C" JNIEXPORT jobject JNICALL
Java_com_kongshang_maichat_MaiCloudDrive_nativeMediaInfo(JNIEnv* env, jclass,
                                                          jstring displayName) {
    std::string name;
    MaiCloudDriveMediaInfo info{};
    if (!readString(env, displayName, &name) ||
        !mai_cloud_drive_media_info(name.c_str(), &info)) return nullptr;
    jclass type = env->FindClass("com/kongshang/maichat/MaiCloudDrive$MediaInfo");
    if (!type) return nullptr;
    jmethodID constructor = env->GetMethodID(type, "<init>",
        "(Ljava/lang/String;Ljava/lang/String;J)V");
    if (!constructor) {
        env->DeleteLocalRef(type);
        return nullptr;
    }
    jstring contentType = env->NewStringUTF(info.content_type_utf8);
    jstring mediaType = env->NewStringUTF(info.media_type_utf8);
    jobject value = contentType && mediaType ? env->NewObject(type, constructor,
        contentType, mediaType, jlong(info.max_size_bytes)) : nullptr;
    if (contentType) env->DeleteLocalRef(contentType);
    if (mediaType) env->DeleteLocalRef(mediaType);
    env->DeleteLocalRef(type);
    return value;
}

extern "C" JNIEXPORT void JNICALL
Java_com_kongshang_maichat_MaiCloudDrive_nativeRequestCancel(JNIEnv*, jclass, jlong handle) {
    mai_cloud_drive_cancel_request(reinterpret_cast<MaiCloudDriveCancel*>(handle));
}

extern "C" JNIEXPORT void JNICALL
Java_com_kongshang_maichat_MaiCloudDrive_nativeDestroyCancel(JNIEnv*, jclass, jlong handle) {
    mai_cloud_drive_cancel_destroy(reinterpret_cast<MaiCloudDriveCancel*>(handle));
}

extern "C" JNIEXPORT jobject JNICALL
Java_com_kongshang_maichat_MaiCloudDrive_nativeRequest(
    JNIEnv* env, jclass, jstring base, jstring token, jbyteArray pem,
    jint action, jstring payload, jstring suffix, jlong handle) {
    MaiCloudDriveResult result{};
    result.status = MAI_CLOUD_DRIVE_INVALID_ARGUMENT;
    Call call;
    if (!readCall(env, base, token, pem, suffix, &call) ||
        !readString(env, payload, &call.payload) || !handle) return complete(env, &call, &result);
    mai_cloud_drive_request(&call.options, static_cast<MaiCloudDriveAction>(action),
        call.payload.c_str(), reinterpret_cast<MaiCloudDriveCancel*>(handle), &result);
    return complete(env, &call, &result);
}

extern "C" JNIEXPORT jobject JNICALL
Java_com_kongshang_maichat_MaiCloudDrive_nativeListCheckpoints(
    JNIEnv* env, jclass, jstring directory) {
    MaiCloudDriveResult result{};
    result.status = MAI_CLOUD_DRIVE_INVALID_ARGUMENT;
    std::string path;
    if (readString(env, directory, &path))
        mai_cloud_drive_list_local_checkpoints(path.c_str(), &result);
    jobject output = makeResult(env, result);
    mai_cloud_drive_result_free(&result);
    return output;
}

extern "C" JNIEXPORT jobject JNICALL
Java_com_kongshang_maichat_MaiCloudDrive_nativeUploadFile(
    JNIEnv* env, jclass, jstring base, jstring token, jbyteArray pem, jstring suffix,
    jstring source, jstring checkpoint, jstring name, jstring type,
    jlong handle, jobject callback) {
    MaiCloudDriveResult result{};
    result.status = MAI_CLOUD_DRIVE_INVALID_ARGUMENT;
    Call call;
    ProgressContext progressContext;
    if (!readCall(env, base, token, pem, suffix, &call) ||
        !readString(env, source, &call.path) ||
        !readString(env, checkpoint, &call.checkpoint) ||
        !readString(env, name, &call.name) ||
        !readString(env, type, &call.type) ||
        !handle || !configureProgress(env, callback, &progressContext))
        return complete(env, &call, &result);
    mai_cloud_drive_upload_resumable(&call.options, call.path.c_str(),
        call.checkpoint.c_str(), call.name.c_str(), call.type.c_str(),
        reinterpret_cast<MaiCloudDriveCancel*>(handle), progress, &progressContext, &result);
    releaseProgress(&progressContext);
    return complete(env, &call, &result);
}

extern "C" JNIEXPORT jobject JNICALL
Java_com_kongshang_maichat_MaiCloudDrive_nativeUploadFd(
    JNIEnv* env, jclass, jstring base, jstring token, jbyteArray pem, jstring suffix,
    jint fd, jstring stableId, jlong bytes, jstring checkpoint, jstring name,
    jstring type, jlong handle, jobject callback) {
    MaiCloudDriveResult result{};
    result.status = MAI_CLOUD_DRIVE_INVALID_ARGUMENT;
    Call call;
    ProgressContext progressContext;
    if (!readCall(env, base, token, pem, suffix, &call) || fd < 0 || bytes <= 0 ||
        !readString(env, stableId, &call.stableId) ||
        !readString(env, checkpoint, &call.checkpoint) ||
        !readString(env, name, &call.name) ||
        !readString(env, type, &call.type) ||
        !handle || !configureProgress(env, callback, &progressContext))
        return complete(env, &call, &result);
    FdContext descriptor{fd};
    MaiCloudDriveSource source{call.stableId.c_str(), static_cast<uint64_t>(bytes),
                               readAt, &descriptor};
    mai_cloud_drive_upload_resumable_source(&call.options, &source,
        call.checkpoint.c_str(), call.name.c_str(), call.type.c_str(),
        reinterpret_cast<MaiCloudDriveCancel*>(handle), progress, &progressContext, &result);
    releaseProgress(&progressContext);
    return complete(env, &call, &result);
}

extern "C" JNIEXPORT jobject JNICALL
Java_com_kongshang_maichat_MaiCloudDrive_nativeStageFd(
    JNIEnv* env, jclass, jint fd, jlong bytes, jstring target,
    jlong handle, jobject callback) {
    MaiCloudDriveResult result{};
    result.status = MAI_CLOUD_DRIVE_INVALID_ARGUMENT;
    std::string path;
    ProgressContext progressContext;
    if (fd >= 0 && bytes > 0 && readString(env, target, &path) && handle &&
        configureProgress(env, callback, &progressContext)) {
        FdContext descriptor{fd};
        mai_cloud_drive_stage_stream(path.c_str(), static_cast<uint64_t>(bytes), readNext,
            &descriptor, reinterpret_cast<MaiCloudDriveCancel*>(handle), progress,
            &progressContext, &result);
        releaseProgress(&progressContext);
    }
    jobject output = makeResult(env, result);
    mai_cloud_drive_result_free(&result);
    return output;
}

extern "C" JNIEXPORT jobject JNICALL
Java_com_kongshang_maichat_MaiCloudDrive_nativeAbort(
    JNIEnv* env, jclass, jstring base, jstring token, jbyteArray pem,
    jstring suffix, jstring checkpoint, jlong handle) {
    MaiCloudDriveResult result{};
    result.status = MAI_CLOUD_DRIVE_INVALID_ARGUMENT;
    Call call;
    if (!readCall(env, base, token, pem, suffix, &call) ||
        !readString(env, checkpoint, &call.checkpoint) || !handle)
        return complete(env, &call, &result);
    mai_cloud_drive_abort_resumable(&call.options, call.checkpoint.c_str(),
        reinterpret_cast<MaiCloudDriveCancel*>(handle), &result);
    return complete(env, &call, &result);
}
