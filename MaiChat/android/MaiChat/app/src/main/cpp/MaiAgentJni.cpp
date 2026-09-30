#include "MaiMobileAgent.h"
#include <android/bitmap.h>
#include <cstdlib>
#include <cstring>
#include <jni.h>
#include <string>

namespace {

struct JniHostToolContext {
  JavaVM *vm = nullptr;
  jobject owner = nullptr;
  jmethodID invoke = nullptr;
};

JNIEnv *attach(JniHostToolContext *context, bool *detachAfter) {
  *detachAfter = false;
  JNIEnv *env = nullptr;
  if (context->vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) ==
      JNI_OK)
    return env;
  if (context->vm->AttachCurrentThread(&env, nullptr) != JNI_OK)
    return nullptr;
  *detachAfter = true;
  return env;
}

const char *hostToolCall(void *opaque, const char *toolName,
                         const char *argumentsJson) {
  auto *context = static_cast<JniHostToolContext *>(opaque);
  bool detachAfter = false;
  JNIEnv *env = attach(context, &detachAfter);
  if (env == nullptr)
    return nullptr;
  const auto makeBytes = [&](const char *value) {
    const jsize size = static_cast<jsize>(std::strlen(value));
    jbyteArray bytes = env->NewByteArray(size);
    if (bytes != nullptr)
      env->SetByteArrayRegion(bytes, 0, size,
                              reinterpret_cast<const jbyte *>(value));
    return bytes;
  };
  jbyteArray tool = makeBytes(toolName);
  jbyteArray arguments = makeBytes(argumentsJson);
  auto *result = static_cast<jbyteArray>(
      env->CallObjectMethod(context->owner, context->invoke, tool, arguments));
  env->DeleteLocalRef(tool);
  env->DeleteLocalRef(arguments);
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    result = nullptr;
  }
  char *owned = nullptr;
  if (result != nullptr) {
    const jsize size = env->GetArrayLength(result);
    owned =
        static_cast<char *>(std::malloc(static_cast<std::size_t>(size) + 1));
    if (owned != nullptr) {
      env->GetByteArrayRegion(result, 0, size,
                              reinterpret_cast<jbyte *>(owned));
      owned[size] = '\0';
    }
    env->DeleteLocalRef(result);
  }
  if (detachAfter)
    context->vm->DetachCurrentThread();
  return owned;
}

void hostToolResponseFree(void *, const char *response) {
  std::free(const_cast<char *>(response));
}

void hostToolContextRelease(void *opaque) {
  auto *context = static_cast<JniHostToolContext *>(opaque);
  bool detachAfter = false;
  JNIEnv *env = attach(context, &detachAfter);
  if (env != nullptr)
    env->DeleteGlobalRef(context->owner);
  if (detachAfter)
    context->vm->DetachCurrentThread();
  delete context;
}

} // namespace

extern "C" JNIEXPORT jlong JNICALL
Java_com_kongshang_maichat_AIAssistantController_nativeCreate(JNIEnv *,
                                                              jclass) {
  return reinterpret_cast<jlong>(maiMobileAgentCreate());
}
extern "C" JNIEXPORT void JNICALL
Java_com_kongshang_maichat_AIAssistantController_nativeDestroy(JNIEnv *, jclass,
                                                               jlong handle) {
  maiMobileAgentDestroy(reinterpret_cast<void *>(handle));
}
extern "C" JNIEXPORT jboolean JNICALL
Java_com_kongshang_maichat_AIAssistantController_nativeSetHostToolHandler(
    JNIEnv *env, jclass, jlong handle, jobject owner) {
  if (handle == 0 || owner == nullptr)
    return JNI_FALSE;
  auto *context = new JniHostToolContext();
  if (env->GetJavaVM(&context->vm) != JNI_OK) {
    delete context;
    return JNI_FALSE;
  }
  context->owner = env->NewGlobalRef(owner);
  jclass ownerClass = env->GetObjectClass(owner);
  context->invoke =
      env->GetMethodID(ownerClass, "onNativeHostTool", "([B[B)[B");
  env->DeleteLocalRef(ownerClass);
  if (context->owner == nullptr || context->invoke == nullptr ||
      env->ExceptionCheck()) {
    env->ExceptionClear();
    if (context->owner != nullptr)
      env->DeleteGlobalRef(context->owner);
    delete context;
    return JNI_FALSE;
  }
  if (maiMobileAgentSetHostToolHandler(
          reinterpret_cast<void *>(handle), context, hostToolCall,
          hostToolResponseFree, hostToolContextRelease) == 0) {
    env->DeleteGlobalRef(context->owner);
    delete context;
    return JNI_FALSE;
  }
  return JNI_TRUE;
}
extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_kongshang_maichat_AIAssistantController_nativeRequest(
    JNIEnv *env, jclass, jlong handle, jbyteArray data) {
  if (!data)
    return nullptr;
  jsize size = env->GetArrayLength(data);
  std::string request(static_cast<size_t>(size), '\0');
  env->GetByteArrayRegion(data, 0, size,
                          reinterpret_cast<jbyte *>(request.data()));
  if (env->ExceptionCheck())
    return nullptr;
  char *response =
      maiMobileAgentRequest(reinterpret_cast<void *>(handle), request.c_str());
  if (!response)
    return nullptr;
  jsize count = static_cast<jsize>(std::strlen(response));
  jbyteArray result = env->NewByteArray(count);
  if (result)
    env->SetByteArrayRegion(result, 0, count,
                            reinterpret_cast<const jbyte *>(response));
  maiMobileAgentFree(response);
  return result;
}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_kongshang_maichat_AIAssistantController_nativeFilterBitmap(
    JNIEnv *env, jclass, jobject bitmap, jstring operation,
    jintArray dimensions) {
  if (bitmap == nullptr || operation == nullptr || dimensions == nullptr ||
      env->GetArrayLength(dimensions) < 2)
    return nullptr;
  AndroidBitmapInfo info = {};
  if (AndroidBitmap_getInfo(env, bitmap, &info) !=
          ANDROID_BITMAP_RESULT_SUCCESS ||
      info.format != ANDROID_BITMAP_FORMAT_RGBA_8888 || info.width < 1 ||
      info.height < 1)
    return nullptr;
  const char *spec = env->GetStringUTFChars(operation, nullptr);
  if (spec == nullptr)
    return nullptr;
  void *input = nullptr;
  if (AndroidBitmap_lockPixels(env, bitmap, &input) !=
      ANDROID_BITMAP_RESULT_SUCCESS) {
    env->ReleaseStringUTFChars(operation, spec);
    return nullptr;
  }
  MaiImageFilterResult result = maiImageFilterRgba(
      static_cast<const unsigned char *>(input), static_cast<int>(info.width),
      static_cast<int>(info.height), static_cast<int>(info.stride), spec);
  AndroidBitmap_unlockPixels(env, bitmap);
  env->ReleaseStringUTFChars(operation, spec);
  if (result.error != nullptr) {
    jclass type = env->FindClass("java/lang/IllegalArgumentException");
    if (type != nullptr)
      env->ThrowNew(type, result.error);
    maiImageFilterFree(result.error);
    return nullptr;
  }
  if (result.rgba == nullptr)
    return nullptr;
  const jint outputDimensions[2] = {result.width, result.height};
  env->SetIntArrayRegion(dimensions, 0, 2, outputDimensions);
  const jsize outputSize = static_cast<jsize>(
      static_cast<long long>(result.width) * result.height * 4);
  jbyteArray output = env->NewByteArray(outputSize);
  if (output != nullptr)
    env->SetByteArrayRegion(output, 0, outputSize,
                            reinterpret_cast<const jbyte *>(result.rgba));
  maiImageFilterFree(result.rgba);
  return output;
}
