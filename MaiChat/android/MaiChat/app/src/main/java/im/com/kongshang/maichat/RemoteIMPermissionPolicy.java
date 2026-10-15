// 文件作用：Android IM 身份、消息同步与本地历史；RemoteIMPermissionPolicy 负责输入筛选、权限判断或展示规则，本文件给出其实现。
// 主要入口：imageReadPermission。
package com.kongshang.maichat;

import android.annotation.SuppressLint;
import android.Manifest;

public final class RemoteIMPermissionPolicy {
    private static final int ANDROID_13_API_LEVEL = 33;

    private RemoteIMPermissionPolicy() {}

    @SuppressLint("InlinedApi")
    public static String imageReadPermission(int sdkInt) {
        return sdkInt >= ANDROID_13_API_LEVEL
            ? Manifest.permission.READ_MEDIA_IMAGES
            : Manifest.permission.READ_EXTERNAL_STORAGE;
    }
}
