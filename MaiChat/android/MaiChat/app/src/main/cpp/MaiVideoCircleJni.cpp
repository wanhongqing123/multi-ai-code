#include "MaiVideoCircleClient.h"
#include "MaiVideoCircleMark.h"

#include <jni.h>

#include <string>
#include <vector>

namespace {

bool readString(JNIEnv* env, jstring value, std::string* output) {
    if (!value) return false;
    const jsize length = env->GetStringLength(value);
    const jchar* data = env->GetStringChars(value, nullptr);
    if (!data) return false;
    output->clear();
    output->reserve(static_cast<size_t>(length) * 3);
    for (jsize index = 0; index < length; ++index) {
        uint32_t codepoint = data[index];
        if (codepoint >= 0xD800 && codepoint <= 0xDBFF && index + 1 < length &&
            data[index + 1] >= 0xDC00 && data[index + 1] <= 0xDFFF) {
            codepoint = 0x10000 + ((codepoint - 0xD800) << 10) +
                        (data[++index] - 0xDC00);
        } else if (codepoint >= 0xD800 && codepoint <= 0xDFFF) {
            env->ReleaseStringChars(value, data);
            return false;
        }
        if (codepoint < 0x80) {
            output->push_back(static_cast<char>(codepoint));
        } else if (codepoint < 0x800) {
            output->push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
            output->push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        } else if (codepoint < 0x10000) {
            output->push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
            output->push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
            output->push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        } else {
            output->push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
            output->push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
            output->push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
            output->push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        }
    }
    env->ReleaseStringChars(value, data);
    return true;
}

bool readPem(JNIEnv* env, jbyteArray value, std::vector<char>* output) {
    if (!value) return false;
    jsize length = env->GetArrayLength(value);
    if (length < 0 || length > 2 * 1024 * 1024) return false;
    output->resize(static_cast<size_t>(length));
    if (length > 0)
        env->GetByteArrayRegion(value, 0, length,
                                reinterpret_cast<jbyte*>(output->data()));
    return !env->ExceptionCheck();
}

jobject makeResult(JNIEnv* env, const MaiVideoCircleResult& result) {
    jclass type = env->FindClass("com/kongshang/maichat/MaiVideoCircle$Result");
    if (!type) return nullptr;
    jmethodID constructor = env->GetMethodID(type, "<init>", "(IJI[B)V");
    if (!constructor) {
        env->DeleteLocalRef(type);
        return nullptr;
    }
    jbyteArray body = env->NewByteArray(static_cast<jsize>(result.response_bytes));
    if (!body) {
        env->DeleteLocalRef(type);
        return nullptr;
    }
    if (result.response_bytes > 0)
        env->SetByteArrayRegion(body, 0, static_cast<jsize>(result.response_bytes),
            reinterpret_cast<const jbyte*>(result.response_json_utf8));
    if (env->ExceptionCheck()) {
        env->DeleteLocalRef(body);
        env->DeleteLocalRef(type);
        return nullptr;
    }
    jobject output = env->NewObject(type, constructor, jint(result.status),
                                    jlong(result.http_status), jint(result.curl_code), body);
    env->DeleteLocalRef(body);
    env->DeleteLocalRef(type);
    return output;
}

struct ProxyContext {
    JNIEnv* env = nullptr;
    jclass type = nullptr;
    jmethodID method = nullptr;
    std::string proxy;
};

int resolveProxy(const char* url, void* opaque, MaiDownloadProxy* output) {
    auto* context = static_cast<ProxyContext*>(opaque);
    jstring address = context->env->NewStringUTF(url);
    if (!address) return 0;
    auto* selected = static_cast<jstring>(
        context->env->CallStaticObjectMethod(context->type, context->method, address));
    context->env->DeleteLocalRef(address);
    if (context->env->ExceptionCheck() || !selected) return 0;
    const bool readable = readString(context->env, selected, &context->proxy);
    context->env->DeleteLocalRef(selected);
    if (!readable) return 0;
    output->url_utf8 = context->proxy.c_str();
    output->username_utf8 = nullptr;
    output->password_utf8 = nullptr;
    return 1;
}

bool configureProxy(JNIEnv* env, ProxyContext* proxy) {
    proxy->env = env;
    proxy->type = env->FindClass("com/kongshang/maichat/MaiVideoCircle");
    if (!proxy->type) return false;
    proxy->method = env->GetStaticMethodID(proxy->type, "proxyForUrl",
        "(Ljava/lang/String;)Ljava/lang/String;");
    if (proxy->method) return true;
    env->DeleteLocalRef(proxy->type);
    proxy->type = nullptr;
    return false;
}

jobject makeMarkResult(JNIEnv* env, const MaiVideoCircleMarkResult& result) {
    jclass type = env->FindClass("com/kongshang/maichat/MaiVideoCircle$MarkResult");
    if (!type) return nullptr;
    jmethodID constructor = env->GetMethodID(type, "<init>",
        "(IJJZZLjava/lang/String;Ljava/lang/String;)V");
    if (!constructor) {
        env->DeleteLocalRef(type);
        return nullptr;
    }
    jstring hash = env->NewStringUTF(result.sha256_hex);
    jstring detail = env->NewStringUTF(result.detail_utf8);
    if (!hash || !detail) {
        if (hash) env->DeleteLocalRef(hash);
        if (detail) env->DeleteLocalRef(detail);
        env->DeleteLocalRef(type);
        return nullptr;
    }
    jobject output = env->NewObject(type, constructor, jint(result.status),
        jlong(result.size_bytes), jlong(result.duration_ms),
        jboolean(result.visible_mark_verified), jboolean(result.metadata_mark_verified),
        hash, detail);
    env->DeleteLocalRef(hash);
    env->DeleteLocalRef(detail);
    env->DeleteLocalRef(type);
    return output;
}

}  // namespace

extern "C" JNIEXPORT jobject JNICALL
Java_com_kongshang_maichat_MaiVideoCircle_nativeInspect(JNIEnv* env, jclass,
                                                         jstring jcard) {
    std::string card;
    MaiVideoCircleResult result{};
    if (!readString(env, jcard, &card)) {
        result.status = MAI_VIDEO_CIRCLE_INVALID_ARGUMENT;
        return makeResult(env, result);
    }
    mai_video_circle_inspect_card(card.c_str(), &result);
    jobject value = makeResult(env, result);
    mai_video_circle_result_free(&result);
    return value;
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_kongshang_maichat_MaiVideoCircle_nativeCreateCancel(JNIEnv*, jclass) {
    return reinterpret_cast<jlong>(mai_video_circle_cancel_create());
}

extern "C" JNIEXPORT void JNICALL
Java_com_kongshang_maichat_MaiVideoCircle_nativeRequestCancel(JNIEnv*, jclass,
                                                               jlong handle) {
    mai_video_circle_cancel_request(reinterpret_cast<MaiVideoCircleCancel*>(handle));
}

extern "C" JNIEXPORT void JNICALL
Java_com_kongshang_maichat_MaiVideoCircle_nativeDestroyCancel(JNIEnv*, jclass,
                                                               jlong handle) {
    mai_video_circle_cancel_destroy(reinterpret_cast<MaiVideoCircleCancel*>(handle));
}

extern "C" JNIEXPORT jobject JNICALL
Java_com_kongshang_maichat_MaiVideoCircle_nativeRequest(
    JNIEnv* env, jclass, jstring jauth, jstring jmedia, jstring jtoken, jbyteArray jpem,
    jint action, jstring jpayload, jlong handle) {
    std::string auth, media, token, payload;
    std::vector<char> pem;
    MaiVideoCircleResult result{};
    result.status = MAI_VIDEO_CIRCLE_INVALID_ARGUMENT;
    if (!readString(env, jauth, &auth) || !readString(env, jmedia, &media) ||
        !readString(env, jtoken, &token) || !readString(env, jpayload, &payload) ||
        !readPem(env, jpem, &pem) || !handle) return makeResult(env, result);
    ProxyContext proxy;
    if (!configureProxy(env, &proxy)) return nullptr;
    MaiVideoCircleOptions options{};
    options.auth_base_url_utf8 = auth.c_str();
    options.media_base_url_utf8 = media.c_str();
    options.account_token_utf8 = token.c_str();
    options.ca_pem = pem.empty() ? nullptr : pem.data();
    options.ca_pem_length = pem.size();
    options.proxy_resolver = resolveProxy;
    options.proxy_context = &proxy;
    mai_video_circle_request(&options, static_cast<MaiVideoCircleAction>(action),
        payload.c_str(), reinterpret_cast<MaiVideoCircleCancel*>(handle), &result);
    env->DeleteLocalRef(proxy.type);
    if (env->ExceptionCheck()) {
        mai_video_circle_result_free(&result);
        return nullptr;
    }
    jobject value = makeResult(env, result);
    mai_video_circle_result_free(&result);
    return value;
}

extern "C" JNIEXPORT jobject JNICALL
Java_com_kongshang_maichat_MaiVideoCircle_nativeUpload(
    JNIEnv* env, jclass, jstring jtoken, jbyteArray jpem, jstring jsigned,
    jstring jsource, jlong bytes, jstring jhostSuffix, jlong handle) {
    std::string token, signedJson, source, hostSuffix;
    std::vector<char> pem;
    MaiVideoCircleResult result{};
    result.status = MAI_VIDEO_CIRCLE_INVALID_ARGUMENT;
    if (!readString(env, jtoken, &token) || !readString(env, jsigned, &signedJson) ||
        !readString(env, jsource, &source) || !readPem(env, jpem, &pem) ||
        bytes <= 0 || !handle) return makeResult(env, result);
    if (jhostSuffix && !readString(env, jhostSuffix, &hostSuffix))
        return makeResult(env, result);
    ProxyContext proxy;
    if (!configureProxy(env, &proxy)) return nullptr;
    MaiVideoCircleOptions options{};
    options.account_token_utf8 = token.c_str();
    options.ca_pem = pem.empty() ? nullptr : pem.data();
    options.ca_pem_length = pem.size();
    options.signed_upload_host_suffix_utf8 = jhostSuffix ? hostSuffix.c_str() : nullptr;
    options.proxy_resolver = resolveProxy;
    options.proxy_context = &proxy;
    mai_video_circle_upload_file(&options, signedJson.c_str(), source.c_str(),
        static_cast<uint64_t>(bytes), reinterpret_cast<MaiVideoCircleCancel*>(handle), &result);
    env->DeleteLocalRef(proxy.type);
    if (env->ExceptionCheck()) {
        mai_video_circle_result_free(&result);
        return nullptr;
    }
    jobject value = makeResult(env, result);
    mai_video_circle_result_free(&result);
    return value;
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_kongshang_maichat_MaiVideoCircle_nativeCreateMarkCancel(JNIEnv*, jclass) {
    return reinterpret_cast<jlong>(mai_video_circle_mark_cancel_create());
}

extern "C" JNIEXPORT void JNICALL
Java_com_kongshang_maichat_MaiVideoCircle_nativeMarkCancel(JNIEnv*, jclass, jlong handle) {
    mai_video_circle_mark_cancel_request(
        reinterpret_cast<MaiVideoCircleMarkCancel*>(handle));
}

extern "C" JNIEXPORT void JNICALL
Java_com_kongshang_maichat_MaiVideoCircle_nativeDestroyMarkCancel(JNIEnv*, jclass,
                                                                   jlong handle) {
    mai_video_circle_mark_cancel_destroy(
        reinterpret_cast<MaiVideoCircleMarkCancel*>(handle));
}

extern "C" JNIEXPORT jobject JNICALL
Java_com_kongshang_maichat_MaiVideoCircle_nativeMark(
    JNIEnv* env, jclass, jstring jsource, jstring jtarget, jlong handle) {
    std::string source, target;
    MaiVideoCircleMarkResult result{};
    result.status = MAI_VIDEO_CIRCLE_MARK_INVALID_ARGUMENT;
    if (!readString(env, jsource, &source) || !readString(env, jtarget, &target) ||
        !handle) return makeMarkResult(env, result);
    mai_video_circle_prepare_marked_mp4(source.c_str(), target.c_str(),
        reinterpret_cast<MaiVideoCircleMarkCancel*>(handle), &result);
    return makeMarkResult(env, result);
}
