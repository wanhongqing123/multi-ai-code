// 文件作用：Android 调试构建；AIAssistantTestActivity 负责提供仅在调试环境启用的辅助入口，本文件给出其实现。
// 类型关系：AIAssistantTestActivity extends Activity；审阅时可据此追踪父类、接口或回调契约。
// 主要入口：onCreate、onDestroy。
package com.kongshang.maichat;

import android.app.Activity;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.os.Bundle;

/** Debug-only host. Production navigation uses the same AIAssistantPanel. */
public final class AIAssistantTestActivity extends Activity {
    AIAssistantPanel panel;
    @Override
    public void onCreate(Bundle saved) {
        super.onCreate(saved);
        String endpoint = getIntent().getStringExtra("model_url");
        if (endpoint == null || endpoint.isEmpty()) endpoint = "http://127.0.0.1:18189";
        panel = new AIAssistantPanel(this, new AIAssistantController(this, endpoint));
        setContentView(panel);
    }
    @Override
    public void onDestroy() {
        panel.closeLocalMusic();
        panel.controller.close();
        super.onDestroy();
    }
    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (resultCode == RESULT_OK) panel.handleActivityResult(requestCode, data);
        else panel.handleActivityCancelled(requestCode);
    }
    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions,
                                           int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode == AIAssistantPanel.REQUEST_CAMERA_PERMISSION)
            panel.onCameraPermission(grantResults.length > 0 &&
                grantResults[0] == PackageManager.PERMISSION_GRANTED);
        if (requestCode == AIAssistantPanel.REQUEST_MUSIC_PERMISSION)
            panel.handleMusicPermission(grantResults.length > 0 &&
                grantResults[0] == PackageManager.PERMISSION_GRANTED);
    }
}
