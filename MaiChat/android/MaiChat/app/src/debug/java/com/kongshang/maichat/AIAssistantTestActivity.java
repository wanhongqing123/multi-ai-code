// 文件作用：Android 调试构建；AIAssistantTestActivity 负责提供仅在调试环境启用的辅助入口，本文件给出其实现。
// 类型关系：AIAssistantTestActivity extends Activity；审阅时可据此追踪父类、接口或回调契约。
// 主要入口：onCreate、onDestroy。
package com.kongshang.maichat;

import android.app.Activity;
import android.os.Bundle;

/** Debug-only host. Production navigation uses the same AIAssistantPanel. */
public final class AIAssistantTestActivity extends Activity {
    AIAssistantPanel panel;
    @Override
    public void onCreate(Bundle saved) {
        super.onCreate(saved);
        panel = new AIAssistantPanel(this, new AIAssistantController(this, "http://127.0.0.1:18189"));
        setContentView(panel);
    }
    @Override
    public void onDestroy() {
        panel.controller.close();
        super.onDestroy();
    }
}
