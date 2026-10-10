package com.kongshang.maichat.tools;

import android.Manifest;
import android.app.NotificationManager;
import android.content.Context;
import android.content.pm.PackageManager;
import android.location.Location;
import android.location.LocationListener;
import android.location.LocationManager;
import android.os.Build;
import android.os.Looper;
import com.kongshang.maichat.MainActivity;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
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

    JSONObject getCurrentLocation() throws Exception {
        if (activity.checkSelfPermission(Manifest.permission.ACCESS_COARSE_LOCATION)
                != PackageManager.PERMISSION_GRANTED &&
            activity.checkSelfPermission(Manifest.permission.ACCESS_FINE_LOCATION)
                != PackageManager.PERMISSION_GRANTED)
            return new JSONObject().put("code", "permission_denied")
                .put("settings_required", true)
                .put("message", "Call mobile_request_permission with location first.");
        if (destroyed || !activity.isHostToolForeground())
            return new JSONObject().put("code", "unavailable")
                .put("message", "Open MaiChat to read the current location.");
        if (Looper.myLooper() == Looper.getMainLooper())
            throw new IllegalStateException("location lookup cannot block the UI thread");
        LocationManager manager = (LocationManager) activity.getSystemService(Context.LOCATION_SERVICE);
        if (manager == null)
            return new JSONObject().put("code", "unavailable");
        final String provider;
        if (manager.isProviderEnabled(LocationManager.NETWORK_PROVIDER))
            provider = LocationManager.NETWORK_PROVIDER;
        else if (activity.checkSelfPermission(Manifest.permission.ACCESS_FINE_LOCATION)
                    == PackageManager.PERMISSION_GRANTED &&
                 manager.isProviderEnabled(LocationManager.GPS_PROVIDER))
            provider = LocationManager.GPS_PROVIDER;
        else return new JSONObject().put("code", "unavailable")
            .put("message", "No permitted location provider is enabled.");

        CountDownLatch latch = new CountDownLatch(1);
        AtomicReference<Location> fix = new AtomicReference<>();
        LocationListener listener = location -> {
            fix.set(location);
            latch.countDown();
        };
        manager.requestSingleUpdate(provider, listener, Looper.getMainLooper());
        boolean received;
        try { received = latch.await(10, TimeUnit.SECONDS); }
        finally { manager.removeUpdates(listener); }
        Location location = fix.get();
        if (!received) return new JSONObject().put("code", "timeout")
            .put("message", "A location fix was not available within 10 seconds.");
        if (location == null || !location.hasAccuracy())
            return new JSONObject().put("code", "unavailable");
        JSONObject result = new JSONObject()
            .put("latitude", location.getLatitude())
            .put("longitude", location.getLongitude())
            .put("accuracy_m", location.getAccuracy())
            .put("timestamp_ms", location.getTime())
            .put("source", LocationManager.GPS_PROVIDER.equals(location.getProvider())
                ? "gps" : "unknown");
        if (location.hasAltitude()) result.put("altitude_m", location.getAltitude());
        return result;
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
            boolean settingsRequired = status.equals("denied") && !hasPhotoRationale();
            return response(permission, status, prompted, settingsRequired,
                access);
        }
        String[] required;
        switch (permission) {
            case "camera": required = new String[]{Manifest.permission.CAMERA}; break;
            case "microphone": required = new String[]{Manifest.permission.RECORD_AUDIO}; break;
            case "location": required = new String[]{Manifest.permission.ACCESS_COARSE_LOCATION};
                break;
            case "contacts": required = new String[]{Manifest.permission.READ_CONTACTS}; break;
            case "phone_number":
                required = new String[]{Manifest.permission.READ_PHONE_NUMBERS}; break;
            case "sms":
                required = new String[]{Manifest.permission.READ_SMS};
                break;
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
                "支持的权限：photos、camera、microphone、location、contacts、calendar、notifications、phone_number、sms");
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
        boolean settingsRequired = !all && (permission.equals("sms") ||
            (prompted && !hasRationale(required)));
        String access = permission.equals("location")
            ? !all ? "none" : activity.checkSelfPermission(Manifest.permission.ACCESS_FINE_LOCATION)
                == PackageManager.PERMISSION_GRANTED ? "precise" : "approximate"
            : "system";
        return response(permission, status, prompted, settingsRequired, access);
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

    private boolean hasPhotoRationale() {
        if (Build.VERSION.SDK_INT < 33)
            return activity.shouldShowRequestPermissionRationale(
                Manifest.permission.READ_EXTERNAL_STORAGE);
        return activity.shouldShowRequestPermissionRationale(
            Manifest.permission.READ_MEDIA_IMAGES) ||
            activity.shouldShowRequestPermissionRationale(Manifest.permission.READ_MEDIA_VIDEO);
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
