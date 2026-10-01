#include <jni.h>
#include <android/bitmap.h>
#include <android/log.h>

#include "MaiImageDecode.h"

#include <cstring>

extern "C" JNIEXPORT jobject JNICALL
Java_com_kongshang_maichat_MessageImageLoader_nativeDecodeImage(JNIEnv* env, jclass,
                                                                  jstring path, jint width,
                                                                  jint height) {
    if (!path) return nullptr;
    const char* name = env->GetStringUTFChars(path, nullptr);
    if (!name) return nullptr;
    MaiImageDecodeResult decoded = maiImageDecodeFile(name, width, height);
    env->ReleaseStringUTFChars(path, name);
    if (!decoded.rgba) {
        __android_log_print(ANDROID_LOG_WARN, "MaiImagePreview",
                            "FFmpeg decode failed: error=%d size=%dx%d", decoded.error_code,
                            width, height);
        return nullptr;
    }

    jclass bitmapClass = env->FindClass("android/graphics/Bitmap");
    jclass configClass = env->FindClass("android/graphics/Bitmap$Config");
    if (!bitmapClass || !configClass) {
        maiImageDecodeFree(decoded.rgba);
        return nullptr;
    }
    jfieldID argbField = env->GetStaticFieldID(
        configClass, "ARGB_8888", "Landroid/graphics/Bitmap$Config;");
    jmethodID create = env->GetStaticMethodID(
        bitmapClass, "createBitmap", "(IILandroid/graphics/Bitmap$Config;)Landroid/graphics/Bitmap;");
    if (!argbField || !create) {
        maiImageDecodeFree(decoded.rgba);
        return nullptr;
    }
    jobject config = env->GetStaticObjectField(configClass, argbField);
    jobject bitmap = env->CallStaticObjectMethod(bitmapClass, create, decoded.width,
                                                 decoded.height, config);
    if (!bitmap || env->ExceptionCheck()) {
        maiImageDecodeFree(decoded.rgba);
        return nullptr;
    }
    AndroidBitmapInfo info{};
    void* pixels = nullptr;
    if (AndroidBitmap_getInfo(env, bitmap, &info) != ANDROID_BITMAP_RESULT_SUCCESS ||
        info.format != ANDROID_BITMAP_FORMAT_RGBA_8888 ||
        AndroidBitmap_lockPixels(env, bitmap, &pixels) != ANDROID_BITMAP_RESULT_SUCCESS) {
        maiImageDecodeFree(decoded.rgba);
        return nullptr;
    }
    for (int row = 0; row < decoded.height; ++row) {
        std::memcpy(static_cast<unsigned char*>(pixels) + row * info.stride,
                    decoded.rgba + row * decoded.stride,
                    static_cast<size_t>(decoded.width) * 4);
    }
    AndroidBitmap_unlockPixels(env, bitmap);
    maiImageDecodeFree(decoded.rgba);
    return bitmap;
}
