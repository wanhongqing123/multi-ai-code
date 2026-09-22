#include "MaiMobileAgent.h"
#include <cstring>
#include <jni.h>
#include <string>
extern "C" JNIEXPORT jlong JNICALL
Java_com_kongshang_maichat_AIAssistantController_nativeCreate(JNIEnv*, jclass) {
    return reinterpret_cast<jlong>(maiMobileAgentCreate());
}
extern "C" JNIEXPORT void JNICALL
Java_com_kongshang_maichat_AIAssistantController_nativeDestroy(JNIEnv*, jclass, jlong handle) {
    maiMobileAgentDestroy(reinterpret_cast<void*>(handle));
}
extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_kongshang_maichat_AIAssistantController_nativeRequest(JNIEnv* env, jclass, jlong handle,
                                                               jbyteArray data) {
    if (!data) return nullptr;
    jsize size = env->GetArrayLength(data);
    std::string request(static_cast<size_t>(size), '\0');
    env->GetByteArrayRegion(data, 0, size, reinterpret_cast<jbyte*>(request.data()));
    if (env->ExceptionCheck()) return nullptr;
    char* response = maiMobileAgentRequest(reinterpret_cast<void*>(handle), request.c_str());
    if (!response) return nullptr;
    jsize count = static_cast<jsize>(std::strlen(response));
    jbyteArray result = env->NewByteArray(count);
    if (result) env->SetByteArrayRegion(result, 0, count, reinterpret_cast<const jbyte*>(response));
    maiMobileAgentFree(response);
    return result;
}
