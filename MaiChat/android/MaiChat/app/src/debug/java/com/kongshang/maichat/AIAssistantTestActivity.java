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
