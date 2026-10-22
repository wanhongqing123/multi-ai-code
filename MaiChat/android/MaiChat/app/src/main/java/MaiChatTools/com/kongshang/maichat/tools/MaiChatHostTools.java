// 文件作用：Android 移动端 Agent 工具与宿主能力；MaiChatHostTools 负责 Agent
// 工具注册、调用与结果返回，本文件给出其实现。 主要入口：onDestroy、onPermissionResult、execute。
// 直接依赖：com.kongshang.maichat.AIAssistantController、com.kongshang.maichat.MainActivity、
// com.kongshang.maichat.RemoteIMSessionController；实现与这些接口协作。
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
    private final MobileContactsTools contacts;
    private final MobileTelephonyTools telephony;
    private final MobileMediaTools media;
    private final MaiChatIMTools im;
    private final MobileSshTools ssh;

    public MaiChatHostTools(MainActivity activity, RemoteIMSessionController session) {
        this.activity = activity;
        this.photos = new MobilePhotoTools(activity);
        this.permissions = new MobilePermissionTools(activity, photos);
        this.contacts = new MobileContactsTools(activity);
        this.telephony = new MobileTelephonyTools(activity);
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
        if (tool.equals("mobile_export_diagnostics"))
            return activity.exportDiagnosticsForAgent();
        if (tool.equals("generate_pdf"))
            return AgentPdfRenderer.render(arguments.optString("html"),
                AIAssistantController.shared(activity).workspacePdfOutputForHost(
                    arguments.optString("output_path")));
        if (tool.equals("mobile_request_permission")) return permissions.request(arguments);
        if (tool.equals("mobile_get_location")) return permissions.getCurrentLocation();
        if (tool.equals("mobile_list_contacts")) return contacts.list(arguments);
        if (tool.equals("mobile_get_contact")) return contacts.get(arguments);
        if (tool.equals("mobile_get_phone_number")) return telephony.getPhoneNumber();
        if (tool.equals("mobile_read_sms")) return telephony.readSms(arguments);
        if (tool.equals("mobile_ssh_password") || tool.equals("mobile_ssh_trust_host"))
            return ssh.execute(tool, arguments);
        if (tool.equals("maichat_play_video") || tool.equals("maichat_video_command") ||
            tool.equals("mobile_take_photo") || tool.equals("mobile_play_local_music"))
            return media.execute(tool, arguments);
        if (tool.startsWith("mobile_")) return photos.execute(tool, arguments);
        return im.execute(tool, arguments);
    }
}
