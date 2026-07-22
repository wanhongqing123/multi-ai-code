package com.multiaicode.remoteim;

import static org.junit.Assert.assertEquals;

import android.Manifest;

import org.junit.Test;

public class RemoteIMPermissionPolicyTest {
    @Test
    public void usesMediaImagesPermissionOnAndroid13AndAbove() {
        assertEquals(
            Manifest.permission.READ_MEDIA_IMAGES,
            RemoteIMPermissionPolicy.imageReadPermission(33)
        );
    }

    @Test
    public void usesExternalStoragePermissionBeforeAndroid13() {
        assertEquals(
            Manifest.permission.READ_EXTERNAL_STORAGE,
            RemoteIMPermissionPolicy.imageReadPermission(32)
        );
    }
}
