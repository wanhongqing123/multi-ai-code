package com.kongshang.maichat.tools;

import com.kongshang.maichat.AIAssistantController;
import com.kongshang.maichat.MainActivity;
import com.kongshang.maichat.RemoteIMSessionController;
import org.json.JSONObject;

/** Dispatches Agent host calls to the owning MaiChat feature. */
public final class MaiChatHostTools {
    private final MainActivity activity;
    private final MobilePhotoTools photos;
    private final MobilePermissionTools permissions;
    private final MobileMediaTools media;
    private final MaiChatIMTools im;
    private final MobileSshTools ssh;

    public MaiChatHostTools(MainActivity activity, RemoteIMSessionController session) {
        this.activity = activity;
        this.photos = new MobilePhotoTools(activity);
        this.permissions = new MobilePermissionTools(activity, photos);
        this.media = new MobileMediaTools(activity);
        this.im = new MaiChatIMTools(activity, session);
        this.ssh = new MobileSshTools(activity);
    }

    public void onDestroy() {
        photos.onDestroy();
        permissions.onDestroy();
    }

    public boolean onPermissionResult(int requestCode) {
        return photos.onPermissionResult(requestCode) ||
            permissions.onPermissionResult(requestCode);
    }

    public Object execute(String tool, JSONObject arguments) throws Exception {
        if (tool.equals("generate_pdf"))
            return AgentPdfRenderer.render(arguments.optString("html"),
                AIAssistantController.shared(activity).workspacePdfOutputForHost(
                    arguments.optString("output_path")));
        if (tool.equals("mobile_request_permission")) return permissions.request(arguments);
        if (tool.equals("mobile_get_location")) return permissions.getCurrentLocation();
        if (tool.equals("mobile_ssh_password") || tool.equals("mobile_ssh_trust_host"))
            return ssh.execute(tool, arguments);
        if (tool.equals("maichat_play_video") || tool.equals("maichat_video_command"))
            return media.execute(tool, arguments);
        if (tool.startsWith("mobile_")) return photos.execute(tool, arguments);
        return im.execute(tool, arguments);
    }
}
