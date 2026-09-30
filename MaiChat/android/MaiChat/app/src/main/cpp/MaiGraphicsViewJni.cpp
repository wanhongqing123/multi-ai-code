#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <jni.h>

#include <cstdarg>
#include <mutex>

#include "MaiGraphicsPresenter.h"
#include "util/base.h"

namespace {

std::mutex sCallbackMutex;
JavaVM* sJavaVm = nullptr;
jclass sViewClass = nullptr;
jmethodID sOnPresented = nullptr;
bool sStarted = false;

void graphicsLog(int level, const char* message, va_list arguments, void*) {
  const int priority = level <= LOG_ERROR     ? ANDROID_LOG_ERROR
                       : level <= LOG_WARNING ? ANDROID_LOG_WARN
                       : level <= LOG_INFO    ? ANDROID_LOG_INFO
                                              : ANDROID_LOG_DEBUG;
  __android_log_vprint(priority, "MaiGraphics", message, arguments);
}

void onPresented(uint64_t viewId, bool success, void*) {
  std::lock_guard<std::mutex> lock(sCallbackMutex);
  if (!sJavaVm || !sViewClass || !sOnPresented) return;
  JNIEnv* env = nullptr;
  bool attached = false;
  if (sJavaVm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) !=
      JNI_OK) {
    if (sJavaVm->AttachCurrentThread(&env, nullptr) != JNI_OK) return;
    attached = true;
  }
  env->CallStaticVoidMethod(sViewClass, sOnPresented,
                            static_cast<jlong>(viewId),
                            static_cast<jboolean>(success));
  if (env->ExceptionCheck()) env->ExceptionClear();
  if (attached) sJavaVm->DetachCurrentThread();
}

void retainWindow(void* window) {
  ANativeWindow_acquire(static_cast<ANativeWindow*>(window));
}

void releaseWindow(void* window) {
  ANativeWindow_release(static_cast<ANativeWindow*>(window));
}

}  // namespace

extern "C" JNIEXPORT jboolean JNICALL
Java_com_kongshang_maichat_MaiGraphicsTextureView_nativeStart(
    JNIEnv* env, jclass type, jstring backend, jstring effectDirectory) {
  if (!backend || !effectDirectory) return JNI_FALSE;
  std::lock_guard<std::mutex> lock(sCallbackMutex);
  if (sStarted) return JNI_TRUE;
  const char* nativeBackend = env->GetStringUTFChars(backend, nullptr);
  if (!nativeBackend) return JNI_FALSE;
  const char* nativeEffects = env->GetStringUTFChars(effectDirectory, nullptr);
  if (!nativeEffects) {
    env->ReleaseStringUTFChars(backend, nativeBackend);
    return JNI_FALSE;
  }
  env->GetJavaVM(&sJavaVm);
  base_set_log_handler(graphicsLog, nullptr);
  sViewClass = static_cast<jclass>(env->NewGlobalRef(type));
  sOnPresented = env->GetStaticMethodID(type, "onNativePresented", "(JZ)V");
  const bool started = sViewClass && sOnPresented &&
                       maiGraphicsPresenterStart(nativeBackend, nativeEffects,
                                                 onPresented, nullptr);
  env->ReleaseStringUTFChars(effectDirectory, nativeEffects);
  env->ReleaseStringUTFChars(backend, nativeBackend);
  if (!started) {
    if (sViewClass) env->DeleteGlobalRef(sViewClass);
    sViewClass = nullptr;
    sOnPresented = nullptr;
    sJavaVm = nullptr;
    return JNI_FALSE;
  }
  sStarted = true;
  return JNI_TRUE;
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_kongshang_maichat_MaiGraphicsTextureView_nativeAttach(
    JNIEnv* env, jclass, jobject surface, jint width, jint height) {
  if (!surface || width <= 0 || height <= 0) return 0;
  ANativeWindow* window = ANativeWindow_fromSurface(env, surface);
  if (!window) return 0;
  const uint64_t id = maiGraphicsPresenterAttach(
      window, static_cast<uint32_t>(width), static_cast<uint32_t>(height),
      retainWindow, releaseWindow);
  ANativeWindow_release(window);
  return static_cast<jlong>(id);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_kongshang_maichat_MaiGraphicsTextureView_nativeShowImage(
    JNIEnv* env, jclass, jlong viewId, jstring path, jboolean fillView) {
  if (!path) return JNI_FALSE;
  const char* nativePath = env->GetStringUTFChars(path, nullptr);
  if (!nativePath) return JNI_FALSE;
  const bool accepted = maiGraphicsPresenterShowImage(
      static_cast<uint64_t>(viewId), nativePath, fillView);
  env->ReleaseStringUTFChars(path, nativePath);
  return accepted ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_kongshang_maichat_MaiGraphicsTextureView_nativeResize(JNIEnv*, jclass,
                                                               jlong viewId,
                                                               jint width,
                                                               jint height) {
  if (width > 0 && height > 0)
    maiGraphicsPresenterResize(static_cast<uint64_t>(viewId),
                               static_cast<uint32_t>(width),
                               static_cast<uint32_t>(height));
}

extern "C" JNIEXPORT void JNICALL
Java_com_kongshang_maichat_MaiGraphicsTextureView_nativeDetach(JNIEnv*, jclass,
                                                               jlong viewId) {
  maiGraphicsPresenterDetach(static_cast<uint64_t>(viewId));
}
