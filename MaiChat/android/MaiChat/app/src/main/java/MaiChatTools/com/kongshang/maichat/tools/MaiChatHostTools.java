package com.kongshang.maichat.tools;

import com.kongshang.maichat.AIAssistantController;
import com.kongshang.maichat.MainActivity;
import com.kongshang.maichat.RemoteIMSessionController;
import org.json.JSONObject;

/** Dispatches Agent host calls to the owning MaiChat feature. */
public final class MaiChatHostTools {
    private final MainActivity activity;
    private final MobilePhotoTools photos;
    private final MaiChatIMTools im;

    public MaiChatHostTools(MainActivity activity, RemoteIMSessionController session) {
        this.activity = activity;
        this.photos = new MobilePhotoTools(activity);
        this.im = new MaiChatIMTools(activity, session);
    }

    public void onDestroy() {
        photos.onDestroy();
    }

    public boolean onPermissionResult(int requestCode) {
        return photos.onPermissionResult(requestCode);
    }

    public Object execute(String tool, JSONObject arguments) throws Exception {
        if (tool.equals("generate_pdf"))
            return AgentPdfRenderer.render(arguments.optString("html"),
                AIAssistantController.shared(activity).workspacePdfOutputForHost(
                    arguments.optString("output_path")));
        if (tool.startsWith("mobile_")) return photos.execute(tool, arguments);
        return im.execute(tool, arguments);
    }
}
