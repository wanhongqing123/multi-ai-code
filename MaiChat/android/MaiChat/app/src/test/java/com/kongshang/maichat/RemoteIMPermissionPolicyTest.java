// 文件作用：Android 本地测试；RemoteIMPermissionPolicyTest 负责覆盖对应类的状态和数据行为，本文件给出其验证。
// 运行边界：仅供自动化验证使用，不作为正式 App 或 Agent 的用户功能入口。
// 覆盖场景：usesMediaImagesPermissionOnAndroid13AndAbove、usesExternalStoragePermissionBeforeAndroid13。
package com.kongshang.maichat;

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
