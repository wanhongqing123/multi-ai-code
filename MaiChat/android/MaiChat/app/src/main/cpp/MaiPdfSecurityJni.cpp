// 文件作用：普通 Java PDF API 以独立 UTF-8 字节调用共用加密核心，不创建含密码的工具 JSON。
#include "MaiMobileAgent.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include <jni.h>

extern "C" JNIEXPORT jbyteArray JNICALL Java_com_kongshang_maichat_tools_MaiPdfSecurity_nativeEncrypt(
    JNIEnv* env, jclass, jbyteArray source, jbyteArray output, jbyteArray password, jboolean replaceExisting) {
    if (source == nullptr || output == nullptr || password == nullptr) return nullptr;
    const jsize sourceSize = env->GetArrayLength(source);
    const jsize outputSize = env->GetArrayLength(output);
    const jsize passwordSize = env->GetArrayLength(password);
    if (sourceSize < 1 || sourceSize > 16384 || outputSize < 1 || outputSize > 16384 || passwordSize < 1 ||
        passwordSize > 127)
        return nullptr;
    try {
        std::string inputPath(static_cast<size_t>(sourceSize), '\0');
        std::string outputPath(static_cast<size_t>(outputSize), '\0');
        std::vector<unsigned char> secret(static_cast<size_t>(passwordSize));
        env->GetByteArrayRegion(source, 0, sourceSize, reinterpret_cast<jbyte*>(&inputPath[0]));
        env->GetByteArrayRegion(output, 0, outputSize, reinterpret_cast<jbyte*>(&outputPath[0]));
        env->GetByteArrayRegion(password, 0, passwordSize, reinterpret_cast<jbyte*>(secret.data()));
        if (env->ExceptionCheck() || inputPath.find('\0') != std::string::npos ||
            outputPath.find('\0') != std::string::npos) {
            std::fill(secret.begin(), secret.end(), 0);
            return nullptr;
        }
        // 显式长度保留 UTF-8；不使用 GetStringUTFChars 的 modified UTF-8，也不截断 NUL。
        char* response = maiMobilePdfEncrypt(inputPath.c_str(), outputPath.c_str(), secret.data(), secret.size(),
                                             replaceExisting == JNI_TRUE ? 1 : 0);
        std::fill(secret.begin(), secret.end(), 0);
        if (response == nullptr) return nullptr;
        const size_t length = std::strlen(response);
        jbyteArray result = length <= 1024 * 1024 ? env->NewByteArray(static_cast<jsize>(length)) : nullptr;
        if (result != nullptr)
            env->SetByteArrayRegion(result, 0, static_cast<jsize>(length), reinterpret_cast<const jbyte*>(response));
        maiMobileAgentFree(response);
        return result;
    } catch (...) {
        // 密码和 qpdf 错误细节不得穿过 JNI 异常/日志；普通 API 用固定失败文案。
        return nullptr;
    }
}
