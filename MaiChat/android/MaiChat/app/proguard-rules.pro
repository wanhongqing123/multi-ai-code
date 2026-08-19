-keep class com.tencent.** { *; }
-dontwarn com.tencent.**

# 腾讯云一句话识别 SDK（官方文档要求）
-keepclasseswithmembernames class * {
    native <methods>;
}
-keep public class com.tencent.cloud.qcloudasrsdk.**
