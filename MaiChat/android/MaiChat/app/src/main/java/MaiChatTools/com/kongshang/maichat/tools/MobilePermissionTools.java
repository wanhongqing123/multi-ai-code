package com.kongshang.maichat.tools;

import android.Manifest;
import android.app.NotificationManager;
import android.content.Context;
import android.content.pm.PackageManager;
import android.os.Build;
import android.os.Looper;
import com.kongshang.maichat.MainActivity;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import org.json.JSONObject;

/** Requests only permissions used by MaiChat features, from the foreground Activity. */
final class MobilePermissionTools {
    private static final int REQUEST_AGENT_PERMISSION = 1010;
    private final MainActivity activity;
    private final MobilePhotoTools photos;
    private volatile boolean destroyed;
    private CountDownLatch pending;

    MobilePermissionTools(MainActivity activity, MobilePhotoTools photos) {
        this.activity = activity;
        this.photos = photos;
    }

    void onDestroy() {
        destroyed = true;
        synchronized (this) {
            if (pending != null) pending.countDown();
            pending = null;
        }
    }

    boolean onPermissionResult(int requestCode) {
        if (requestCode != REQUEST_AGENT_PERMISSION) return false;
        synchronized (this) {
            if (pending != null) pending.countDown();
            pending = null;
        }
        return true;
    }

    JSONObject request(JSONObject arguments) throws Exception {
        String permission = arguments.optString("permission", "").trim();
        if (permission.equals("photos")) {
            String access;
            boolean prompted = false;
            try {
                prompted = !hasAllPhotoAccess();
                access = photos.agentPhotoAccess();
            } catch (SecurityException denied) {
                access = "denied";
            }
            String status = access.equals("full") ? "granted"
                : access.equals("denied") ? "denied" : "limited";
            boolean settingsRequired = status.equals("denied") ||
                (Build.VERSION.SDK_INT >= 33 && access.equals("images_only") &&
                    !activity.shouldShowRequestPermissionRationale(
                        Manifest.permission.READ_MEDIA_VIDEO));
            return response(permission, status, prompted, settingsRequired,
                access);
        }
        String[] required;
        switch (permission) {
            case "camera": required = new String[]{Manifest.permission.CAMERA}; break;
            case "microphone": required = new String[]{Manifest.permission.RECORD_AUDIO}; break;
            case "location": required = new String[]{Manifest.permission.ACCESS_FINE_LOCATION,
                Manifest.permission.ACCESS_COARSE_LOCATION}; break;
            case "contacts": required = new String[]{Manifest.permission.READ_CONTACTS}; break;
            case "calendar": required = new String[]{Manifest.permission.READ_CALENDAR}; break;
            case "notifications":
                if (Build.VERSION.SDK_INT < 33) {
                    NotificationManager manager = (NotificationManager)
                        activity.getSystemService(Context.NOTIFICATION_SERVICE);
                    boolean allowed = manager == null || manager.areNotificationsEnabled();
                    return response(permission, allowed ? "granted" : "denied", false,
                        !allowed, "system_settings");
                }
                required = new String[]{Manifest.permission.POST_NOTIFICATIONS};
                break;
            default: throw new IllegalArgumentException(
                "支持的权限：photos、camera、microphone、location、contacts、calendar、notifications");
        }
        boolean prompted = !hasAll(required);
        if (prompted) {
            if (destroyed || !activity.isHostToolForeground())
                throw new IllegalStateException("请打开 MaiChat 后再申请系统权限");
            if (Looper.myLooper() == Looper.getMainLooper())
                throw new IllegalStateException("权限申请不能阻塞界面线程");
            CountDownLatch latch = new CountDownLatch(1);
            synchronized (this) {
                if (pending != null) throw new IllegalStateException("正在等待另一项权限授权");
                pending = latch;
            }
            activity.runOnUiThread(() -> {
                if (destroyed) latch.countDown();
                else activity.requestPermissions(required, REQUEST_AGENT_PERMISSION);
            });
            try {
                if (!latch.await(120, TimeUnit.SECONDS))
                    throw new IllegalStateException("等待系统权限超时");
            } finally {
                synchronized (this) {
                    if (pending == latch) pending = null;
                }
            }
        }
        boolean all = hasAll(required);
        boolean some = hasAny(required);
        String status = all ? "granted" : some ? "limited" : "denied";
        boolean settingsRequired = !all && prompted && !hasRationale(required);
        return response(permission, status, prompted, settingsRequired,
            permission.equals("location") ? "when_in_use" : "system");
    }

    private boolean hasAllPhotoAccess() {
        if (Build.VERSION.SDK_INT < 33)
            return activity.checkSelfPermission(Manifest.permission.READ_EXTERNAL_STORAGE)
                == PackageManager.PERMISSION_GRANTED;
        return activity.checkSelfPermission(Manifest.permission.READ_MEDIA_IMAGES)
                == PackageManager.PERMISSION_GRANTED &&
            activity.checkSelfPermission(Manifest.permission.READ_MEDIA_VIDEO)
                == PackageManager.PERMISSION_GRANTED;
    }

    private boolean hasAll(String[] permissions) {
        for (String permission : permissions)
            if (activity.checkSelfPermission(permission) != PackageManager.PERMISSION_GRANTED)
                return false;
        return true;
    }

    private boolean hasAny(String[] permissions) {
        for (String permission : permissions)
            if (activity.checkSelfPermission(permission) == PackageManager.PERMISSION_GRANTED)
                return true;
        return false;
    }

    private boolean hasRationale(String[] permissions) {
        for (String permission : permissions)
            if (activity.shouldShowRequestPermissionRationale(permission)) return true;
        return false;
    }

    private static JSONObject response(String permission, String status, boolean prompted,
                                       boolean settingsRequired, String access) throws Exception {
        return new JSONObject().put("permission", permission).put("status", status)
            .put("prompted", prompted).put("settings_required", settingsRequired)
            .put("access", access);
    }
}
