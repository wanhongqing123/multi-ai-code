#include "MaiCurlDownload.h"

#include <jni.h>

#include <string>
#include <vector>

namespace {

bool readString(JNIEnv* env, jstring value, std::string* output) {
    if (value == nullptr) return false;
    const char* text = env->GetStringUTFChars(value, nullptr);
    if (text == nullptr) return false;
    output->assign(text);
    env->ReleaseStringUTFChars(value, text);
    return true;
}

jobject makeResult(JNIEnv* env, const MaiDownloadResult& result) {
    jclass type = env->FindClass("com/kongshang/maichat/MaiChatAttachmentDownload$Result");
    if (type == nullptr) return nullptr;
    jmethodID constructor = env->GetMethodID(type, "<init>", "(IJIJ)V");
    if (constructor == nullptr) {
        env->DeleteLocalRef(type);
        return nullptr;
    }
    jobject value = env->NewObject(type, constructor, jint(result.status), jlong(result.http_status),
                                   jint(result.curl_code), jlong(result.bytes));
    env->DeleteLocalRef(type);
    return value;
}

struct ProxyContext {
    JNIEnv* env = nullptr;
    jclass type = nullptr;
    jmethodID method = nullptr;
    std::string currentProxy;
};

int resolveProxy(const char* url, void* opaque, MaiDownloadProxy* output) {
    auto* context = static_cast<ProxyContext*>(opaque);
    jstring address = context->env->NewStringUTF(url);
    if (address == nullptr) return 0;
    auto* selected = static_cast<jstring>(
        context->env->CallStaticObjectMethod(context->type, context->method, address));
    context->env->DeleteLocalRef(address);
    if (context->env->ExceptionCheck() || selected == nullptr) return 0;
    const bool readable = readString(context->env, selected, &context->currentProxy);
    context->env->DeleteLocalRef(selected);
    if (!readable) return 0;
    output->url_utf8 = context->currentProxy.c_str();
    output->username_utf8 = nullptr;
    output->password_utf8 = nullptr;
    return 1;
}

}  // namespace

extern "C" JNIEXPORT jlong JNICALL
Java_com_kongshang_maichat_MaiChatAttachmentDownload_nativeCreateCancel(JNIEnv*, jclass) {
    return reinterpret_cast<jlong>(mai_download_cancel_create());
}

extern "C" JNIEXPORT void JNICALL
Java_com_kongshang_maichat_MaiChatAttachmentDownload_nativeRequestCancel(JNIEnv*, jclass,
                                                                            jlong handle) {
    mai_download_cancel_request(reinterpret_cast<MaiDownloadCancel*>(handle));
}

extern "C" JNIEXPORT void JNICALL
Java_com_kongshang_maichat_MaiChatAttachmentDownload_nativeDestroyCancel(JNIEnv*, jclass,
                                                                            jlong handle) {
    mai_download_cancel_destroy(reinterpret_cast<MaiDownloadCancel*>(handle));
}

extern "C" JNIEXPORT jobject JNICALL
Java_com_kongshang_maichat_MaiChatAttachmentDownload_nativeDownload(
    JNIEnv* env, jclass, jstring jurl, jstring jtarget, jlong maximumBytes,
    jbyteArray jcaPem, jlong jcancelHandle) {
    std::string url;
    std::string target;
    MaiDownloadResult result{};
    if (!readString(env, jurl, &url) || !readString(env, jtarget, &target) ||
        jcaPem == nullptr || maximumBytes <= 0 || jcancelHandle == 0) {
        result.status = MAI_DOWNLOAD_INVALID_ARGUMENT;
        return makeResult(env, result);
    }
    const jsize caLength = env->GetArrayLength(jcaPem);
    if (caLength < 0 || caLength > 2 * 1024 * 1024) {
        result.status = MAI_DOWNLOAD_INVALID_ARGUMENT;
        return makeResult(env, result);
    }
    std::vector<char> ca(static_cast<size_t>(caLength));
    if (caLength > 0) env->GetByteArrayRegion(jcaPem, 0, caLength,
                                              reinterpret_cast<jbyte*>(ca.data()));
    if (env->ExceptionCheck()) return nullptr;
    ProxyContext proxyContext;
    proxyContext.env = env;
    proxyContext.type = env->FindClass("com/kongshang/maichat/MaiChatAttachmentDownload");
    if (proxyContext.type == nullptr) return nullptr;
    proxyContext.method = env->GetStaticMethodID(proxyContext.type, "proxyForUrl",
                                                  "(Ljava/lang/String;)Ljava/lang/String;");
    if (proxyContext.method == nullptr) {
        env->DeleteLocalRef(proxyContext.type);
        return nullptr;
    }
    MaiDownloadOptions options{};
    options.url_utf8 = url.c_str();
    options.target_path_utf8 = target.c_str();
    options.max_bytes = static_cast<uint64_t>(maximumBytes);
    options.ca_pem = ca.empty() ? nullptr : ca.data();
    options.ca_pem_length = ca.size();
    options.proxy_resolver = resolveProxy;
    options.proxy_context = &proxyContext;
    mai_download_file(&options, reinterpret_cast<MaiDownloadCancel*>(jcancelHandle), &result);
    env->DeleteLocalRef(proxyContext.type);
    if (env->ExceptionCheck()) return nullptr;
    return makeResult(env, result);
}
