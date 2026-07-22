package com.kongshang.maichat;

import android.Manifest;

public final class RemoteIMPermissionPolicy {
    private static final int ANDROID_13_API_LEVEL = 33;

    private RemoteIMPermissionPolicy() {}

    public static String imageReadPermission(int sdkInt) {
        return sdkInt >= ANDROID_13_API_LEVEL
            ? Manifest.permission.READ_MEDIA_IMAGES
            : Manifest.permission.READ_EXTERNAL_STORAGE;
    }
}
