package com.kongshang.maichat.tools;

import com.kongshang.maichat.AIAssistantController;
import com.kongshang.maichat.MainActivity;
import android.Manifest;
import android.content.ContentUris;
import android.content.ContentValues;
import android.content.pm.PackageManager;
import android.database.Cursor;
import android.graphics.BitmapFactory;
import android.media.MediaMetadataRetriever;
import android.net.Uri;
import android.os.Build;
import android.os.Environment;
import android.provider.MediaStore;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import org.json.JSONArray;
import org.json.JSONObject;

final class MobilePhotoTools {
    private static final int REQUEST_AGENT_PHOTOS = 1007;
    private final MainActivity activity;
    private volatile boolean destroyed;
    private CountDownLatch pendingAgentPhotoPermission;

    MobilePhotoTools(MainActivity activity) {
        this.activity = activity;
    }

    void onDestroy() {
        destroyed = true;
        synchronized (this) {
            if (pendingAgentPhotoPermission != null) {
                pendingAgentPhotoPermission.countDown();
                pendingAgentPhotoPermission = null;
            }
        }
    }

    boolean onPermissionResult(int requestCode) {
        if (requestCode != REQUEST_AGENT_PHOTOS) return false;
        synchronized (this) {
            if (pendingAgentPhotoPermission != null) {
                pendingAgentPhotoPermission.countDown();
                pendingAgentPhotoPermission = null;
            }
        }
        return true;
    }

    Object execute(String tool, JSONObject arguments) throws Exception {
        switch (tool) {
            case "mobile_list_photos": return agentListPhotos(arguments);
            case "mobile_list_albums": return agentListAlbums();
            case "mobile_read_photo": return agentReadPhoto(arguments);
            case "mobile_export_photo_original": return agentExportPhotoOriginal(arguments);
            case "mobile_export_media_original": return agentExportPhotoOriginal(arguments);
            case "mobile_save_image": return agentSaveImage(arguments);
            case "mobile_save_video": return agentSaveVideo(arguments);
            case "mobile_transform_image": return agentTransformImage(arguments);
            case "mobile_beautify_image":
                arguments.put("operation", "beautify");
                return agentTransformImage(arguments);
            case "mobile_detect_faces": return agentDetectFaces(arguments);
            case "mobile_segment_person": return agentSegmentPerson(arguments);
            case "mobile_preview_image": return agentPreviewImage(arguments);
            case "mobile_photos_copy_to_album": return agentCopyPhotosToAlbum(arguments);
            default: throw new IllegalArgumentException("未知图片工具：" + tool);
        }
    }

    String agentPhotoAccess() throws Exception {
        String imagePermission = Build.VERSION.SDK_INT >= 33
            ? Manifest.permission.READ_MEDIA_IMAGES : Manifest.permission.READ_EXTERNAL_STORAGE;
        boolean imagesGranted = activity.checkSelfPermission(imagePermission)
            == PackageManager.PERMISSION_GRANTED;
        boolean videosGranted = Build.VERSION.SDK_INT < 33 || activity.checkSelfPermission(
            Manifest.permission.READ_MEDIA_VIDEO) == PackageManager.PERMISSION_GRANTED;
        if (imagesGranted && videosGranted) return "full";
        if (Build.VERSION.SDK_INT >= 34 && activity.checkSelfPermission(
                Manifest.permission.READ_MEDIA_VISUAL_USER_SELECTED) == PackageManager.PERMISSION_GRANTED)
            return "limited";
        if (destroyed || !activity.isHostToolForeground()) throw new IllegalStateException(
            "请打开 MaiChat，在系统权限提示中允许相册访问");
        CountDownLatch pending = new CountDownLatch(1);
        synchronized (this) {
            if (pendingAgentPhotoPermission != null)
                throw new IllegalStateException("正在等待相册权限确认");
            pendingAgentPhotoPermission = pending;
        }
        activity.runOnUiThread(() -> {
            if (destroyed) {
                pending.countDown();
                return;
            }
            String[] permissions = Build.VERSION.SDK_INT >= 34
                ? new String[]{Manifest.permission.READ_MEDIA_IMAGES,
                    Manifest.permission.READ_MEDIA_VIDEO,
                    Manifest.permission.READ_MEDIA_VISUAL_USER_SELECTED}
                : Build.VERSION.SDK_INT >= 33
                    ? new String[]{Manifest.permission.READ_MEDIA_IMAGES,
                        Manifest.permission.READ_MEDIA_VIDEO}
                    : new String[]{imagePermission};
            activity.requestPermissions(permissions, REQUEST_AGENT_PHOTOS);
        });
        try {
            if (!pending.await(120, TimeUnit.SECONDS))
                throw new IllegalStateException("等待相册权限超时");
        } finally {
            synchronized (this) {
                if (pendingAgentPhotoPermission == pending)
                    pendingAgentPhotoPermission = null;
            }
        }
        imagesGranted = activity.checkSelfPermission(imagePermission)
            == PackageManager.PERMISSION_GRANTED;
        videosGranted = Build.VERSION.SDK_INT < 33 || activity.checkSelfPermission(
            Manifest.permission.READ_MEDIA_VIDEO) == PackageManager.PERMISSION_GRANTED;
        if (imagesGranted && videosGranted) return "full";
        if (Build.VERSION.SDK_INT >= 34 && activity.checkSelfPermission(
                Manifest.permission.READ_MEDIA_VISUAL_USER_SELECTED) == PackageManager.PERMISSION_GRANTED)
            return "limited";
        if (imagesGranted) return "images_only";
        if (videosGranted) return "videos_only";
        throw new SecurityException("没有获得相册访问权限");
    }

    private JSONObject agentListPhotos(JSONObject arguments) throws Exception {
        String access = agentPhotoAccess();
        int offset = Math.max(0, arguments.optInt("offset", 0));
        int limit = Math.min(100, Math.max(1, arguments.optInt("limit", 50)));
        String albumId = arguments.optString("album_id", "").trim();
        String selection = MediaStore.Files.FileColumns.MEDIA_TYPE + " IN (?, ?)";
        String[] selectionArgs = albumId.isEmpty() || albumId.equals("all")
            ? new String[]{"1", "3"} : new String[]{"1", "3", albumId};
        if (selectionArgs.length == 3) selection += " AND " + MediaStore.Images.Media.BUCKET_ID + "=?";
        String[] projection = {MediaStore.Images.Media._ID,
            MediaStore.Images.Media.DATE_TAKEN, MediaStore.Images.Media.DATE_ADDED,
            MediaStore.Images.Media.WIDTH, MediaStore.Images.Media.HEIGHT,
            MediaStore.Images.Media.BUCKET_ID,
            MediaStore.Files.FileColumns.MEDIA_TYPE,
            MediaStore.MediaColumns.MIME_TYPE,
            MediaStore.Video.Media.DURATION};
        JSONArray items = new JSONArray();
        int total;
        try (Cursor cursor = activity.getContentResolver().query(
                agentVisualCollection(), projection,
                selection, selectionArgs,
                MediaStore.Images.Media.DATE_TAKEN + " DESC, " + MediaStore.Images.Media._ID + " DESC")) {
            if (cursor == null) throw new IllegalStateException("无法读取系统相册");
            total = cursor.getCount();
            if (offset < total && cursor.moveToPosition(offset)) {
                do {
                    long created = cursor.getLong(1);
                    if (created <= 0) created = cursor.getLong(2) * 1000;
                    int mediaType = cursor.getInt(6);
                    items.put(new JSONObject()
                        .put("id", Long.toString(cursor.getLong(0)))
                        .put("mediaType", mediaType == MediaStore.Files.FileColumns.MEDIA_TYPE_VIDEO
                            ? "video" : "photo")
                        .put("created_at_ms", created)
                        .put("width", cursor.getInt(3))
                        .put("height", cursor.getInt(4))
                        .put("album_id", cursor.getString(5))
                        .put("mime_type", cursor.getString(7))
                        .put("duration_ms", mediaType == MediaStore.Files.FileColumns.MEDIA_TYPE_VIDEO
                            ? cursor.getLong(8) : 0));
                } while (items.length() < limit && cursor.moveToNext());
            }
        }
        return new JSONObject().put("access", access).put("total", total)
            .put("offset", offset).put("items", items);
    }

    private JSONObject agentListAlbums() throws Exception {
        String access = agentPhotoAccess();
        Map<String, Integer> counts = new LinkedHashMap<>();
        Map<String, String> titles = new LinkedHashMap<>();
        String[] columns = {MediaStore.Images.Media.BUCKET_ID,
            MediaStore.Images.Media.BUCKET_DISPLAY_NAME};
        try (Cursor cursor = activity.getContentResolver().query(
                agentVisualCollection(), columns,
                MediaStore.Files.FileColumns.MEDIA_TYPE + " IN (?, ?)",
                new String[]{"1", "3"}, null)) {
            if (cursor == null) throw new IllegalStateException("无法读取系统相簿");
            while (cursor.moveToNext()) {
                String id = cursor.getString(0);
                if (id == null || id.isEmpty()) continue;
                counts.put(id, counts.getOrDefault(id, 0) + 1);
                titles.putIfAbsent(id, cursor.getString(1));
            }
        }
        JSONArray albums = new JSONArray();
        albums.put(new JSONObject().put("id", "all").put("name", "所有照片"));
        for (String id : counts.keySet()) {
            if (albums.length() >= 201) break;
            albums.put(new JSONObject().put("id", id).put("name", titles.get(id))
                .put("count", counts.get(id)));
        }
        return new JSONObject().put("access", access).put("albums", albums);
    }

    private JSONObject agentReadPhoto(JSONObject arguments) throws Exception {
        agentPhotoAccess();
        String rawId = arguments.optString("id", "").trim();
        long id;
        try { id = Long.parseLong(rawId); }
        catch (NumberFormatException error) { throw new IllegalArgumentException("无效的照片 ID"); }
        if (id <= 0) throw new IllegalArgumentException("无效的照片 ID");
        if (agentMediaType(id) != MediaStore.Files.FileColumns.MEDIA_TYPE_IMAGE)
            throw new IllegalArgumentException("视频请使用 mobile_export_photo_original 获取原文件");
        Uri uri = ContentUris.withAppendedId(agentPhotoCollection(), id);
        AIAssistantController.ImportedFile file =
            AIAssistantController.shared(activity).importPhotoForHost(uri);
        return new JSONObject().put("id", rawId).put("path", file.relativePath)
            .put("mime_type", file.mimeType).put("next_tool", "mobile_preview_image")
            .put("vision_tool", "view_image");
    }

    private JSONObject agentExportPhotoOriginal(JSONObject arguments) throws Exception {
        agentPhotoAccess();
        String rawId = arguments.optString("id", "").trim();
        long id;
        try { id = Long.parseLong(rawId); }
        catch (NumberFormatException error) { throw new IllegalArgumentException("无效的照片 ID"); }
        if (id <= 0) throw new IllegalArgumentException("无效的照片 ID");
        int mediaType = agentMediaType(id);
        Uri uri = ContentUris.withAppendedId(
            mediaType == MediaStore.Files.FileColumns.MEDIA_TYPE_VIDEO
                ? agentVideoCollection() : agentPhotoCollection(), id);
        AIAssistantController.ImportedFile file =
            AIAssistantController.shared(activity).importOriginalMediaForHost(uri);
        return new JSONObject().put("id", rawId).put("path", file.relativePath)
            .put("mime_type", file.mimeType)
            .put("mediaType", mediaType == MediaStore.Files.FileColumns.MEDIA_TYPE_VIDEO
                ? "video" : "photo");
    }

    private JSONObject agentSaveVideo(JSONObject arguments) throws Exception {
        if (Build.VERSION.SDK_INT < 29)
            throw new IllegalStateException("当前 Android 版本无法安全保存到系统图库");
        String path = arguments.optString("path", "");
        File source = AIAssistantController.shared(activity).workspaceFile(path);
        if (source == null || !source.isFile() || source.length() < 1
                || source.length() > 2L * 1024 * 1024 * 1024)
            throw new IllegalArgumentException("视频必须是 App 目录内不超过 2 GB 的文件");
        String lowerName = source.getName().toLowerCase(java.util.Locale.ROOT);
        String extension;
        String mime;
        if (lowerName.endsWith(".mp4")) {
            extension = "mp4";
            mime = "video/mp4";
        } else if (lowerName.endsWith(".mov")) {
            extension = "mov";
            mime = "video/quicktime";
        } else if (lowerName.endsWith(".m4v")) {
            extension = "m4v";
            mime = "video/x-m4v";
        } else {
            throw new IllegalArgumentException("仅支持 MP4、MOV 或 M4V 视频");
        }
        MediaMetadataRetriever metadataReader = new MediaMetadataRetriever();
        try {
            metadataReader.setDataSource(source.getPath());
            if (!"yes".equalsIgnoreCase(metadataReader.extractMetadata(
                    MediaMetadataRetriever.METADATA_KEY_HAS_VIDEO)))
                throw new IllegalArgumentException("文件没有视频轨道");
        } finally {
            metadataReader.release();
        }
        ContentValues metadata = new ContentValues();
        metadata.put(MediaStore.MediaColumns.DISPLAY_NAME,
            "MaiChat-" + java.util.UUID.randomUUID() + "." + extension);
        metadata.put(MediaStore.MediaColumns.MIME_TYPE, mime);
        metadata.put(MediaStore.MediaColumns.RELATIVE_PATH,
            Environment.DIRECTORY_MOVIES + "/MaiChat/");
        metadata.put(MediaStore.MediaColumns.IS_PENDING, 1);
        Uri collection = MediaStore.Video.Media.getContentUri(MediaStore.VOLUME_EXTERNAL_PRIMARY);
        Uri created = activity.getContentResolver().insert(collection, metadata);
        if (created == null) throw new IOException("无法创建系统图库视频");
        try {
            try (InputStream input = new FileInputStream(source);
                 OutputStream output = activity.getContentResolver().openOutputStream(created)) {
                if (output == null) throw new IOException("无法写入系统图库视频");
                byte[] buffer = new byte[64 * 1024];
                int count;
                while ((count = input.read(buffer)) >= 0)
                    output.write(buffer, 0, count);
            }
            ContentValues visible = new ContentValues();
            visible.put(MediaStore.MediaColumns.IS_PENDING, 0);
            if (activity.getContentResolver().update(created, visible, null, null) < 1)
                throw new IOException("无法完成系统图库视频");
        } catch (Exception error) {
            try { activity.getContentResolver().delete(created, null, null); }
            catch (Exception ignored) { /* Only the newly created video is removed. */ }
            throw error;
        }
        return new JSONObject().put("saved", true).put("id", Long.toString(ContentUris.parseId(created)))
            .put("uri", created.toString()).put("source_path", path).put("bytes", source.length());
    }

    private JSONObject agentSaveImage(JSONObject arguments) throws Exception {
        if (Build.VERSION.SDK_INT < 29)
            throw new IllegalStateException("当前 Android 版本无法安全保存到系统图库");
        File source = AIAssistantController.shared(activity)
            .workspaceImageForHost(arguments.optString("path", ""));
        if (source.length() < 1 || source.length() > 50L * 1024 * 1024)
            throw new IllegalArgumentException("图片大小必须在 50 MB 以内");
        BitmapFactory.Options bounds = new BitmapFactory.Options();
        bounds.inJustDecodeBounds = true;
        BitmapFactory.decodeFile(source.getPath(), bounds);
        String mime = bounds.outMimeType;
        if (bounds.outWidth < 1 || bounds.outHeight < 1 || mime == null
                || !(mime.equals("image/jpeg") || mime.equals("image/png")
                    || mime.equals("image/webp") || mime.equals("image/heif")
                    || mime.equals("image/heic")))
            throw new IllegalArgumentException("需要有效的 JPEG、PNG、WebP 或 HEIF 图片");
        String extension = mime.equals("image/png") ? "png"
            : mime.equals("image/webp") ? "webp"
            : mime.equals("image/heif") || mime.equals("image/heic") ? "heic" : "jpg";
        ContentValues metadata = new ContentValues();
        metadata.put(MediaStore.MediaColumns.DISPLAY_NAME,
            "MaiChat-" + java.util.UUID.randomUUID() + "." + extension);
        metadata.put(MediaStore.MediaColumns.MIME_TYPE, mime);
        metadata.put(MediaStore.MediaColumns.RELATIVE_PATH,
            Environment.DIRECTORY_PICTURES + "/MaiChat/");
        metadata.put(MediaStore.MediaColumns.IS_PENDING, 1);
        Uri collection = MediaStore.Images.Media.getContentUri(MediaStore.VOLUME_EXTERNAL_PRIMARY);
        Uri created = activity.getContentResolver().insert(collection, metadata);
        if (created == null) throw new IOException("无法创建系统图库图片");
        try {
            try (InputStream input = new FileInputStream(source);
                 OutputStream output = activity.getContentResolver().openOutputStream(created)) {
                if (output == null) throw new IOException("无法写入系统图库图片");
                byte[] buffer = new byte[8192];
                int count;
                while ((count = input.read(buffer)) >= 0)
                    output.write(buffer, 0, count);
            }
            ContentValues visible = new ContentValues();
            visible.put(MediaStore.MediaColumns.IS_PENDING, 0);
            if (activity.getContentResolver().update(created, visible, null, null) < 1)
                throw new IOException("无法完成系统图库图片");
        } catch (Exception error) {
            try { activity.getContentResolver().delete(created, null, null); }
            catch (Exception ignored) { /* Only a newly created, incomplete image is removed. */ }
            throw error;
        }
        return new JSONObject().put("saved", true).put("id", Long.toString(ContentUris.parseId(created)))
            .put("uri", created.toString()).put("source_path", arguments.optString("path"));
    }

    private JSONObject agentTransformImage(JSONObject arguments) throws Exception {
        AIAssistantController.ImportedFile file = AIAssistantController.shared(activity)
            .transformImageForHost(arguments);
        File result = AIAssistantController.shared(activity).workspaceImageForHost(file.relativePath);
        BitmapFactory.Options bounds = new BitmapFactory.Options();
        bounds.inJustDecodeBounds = true;
        BitmapFactory.decodeFile(result.getPath(), bounds);
        return new JSONObject().put("path", file.relativePath)
            .put("mime_type", file.mimeType).put("width", bounds.outWidth)
            .put("height", bounds.outHeight).put("next_tool", "mobile_preview_image");
    }

    private JSONObject agentImageInfo(JSONObject arguments) throws Exception {
        File source = AIAssistantController.shared(activity)
            .workspaceImageForHost(arguments.optString("path", ""));
        if (source.length() < 1 || source.length() > 50L * 1024 * 1024)
            throw new IllegalArgumentException("图片大小必须在 50 MB 以内");
        BitmapFactory.Options bounds = new BitmapFactory.Options();
        bounds.inJustDecodeBounds = true;
        BitmapFactory.decodeFile(source.getPath(), bounds);
        if (bounds.outWidth < 1 || bounds.outHeight < 1 || bounds.outMimeType == null)
            throw new IllegalArgumentException("图片格式无法读取");
        return new JSONObject().put("path", arguments.optString("path"))
            .put("mime_type", bounds.outMimeType).put("bytes", source.length())
            .put("width", bounds.outWidth).put("height", bounds.outHeight);
    }

    private JSONObject agentPreviewImage(JSONObject arguments) throws Exception {
        JSONObject info = agentImageInfo(arguments);
        if (destroyed || !activity.isHostToolForeground())
            throw new IllegalStateException("请打开 MaiChat 后再预览图片");
        File source = AIAssistantController.shared(activity)
            .workspaceImageForHost(arguments.optString("path", ""));
        activity.runOnUiThread(() -> {
            if (!destroyed && activity.isHostToolForeground()) activity.showFullScreenImage(source.getPath());
        });
        return new JSONObject().put("opened", true).put("path", info.getString("path"));
    }

    private JSONObject agentDetectFaces(JSONObject arguments) throws Exception {
        File source = AIAssistantController.shared(activity)
            .workspaceImageForHost(arguments.optString("path", ""));
        return MobileImageVision.detectFaces(source)
            .put("source_path", arguments.optString("path"));
    }

    private JSONObject agentSegmentPerson(JSONObject arguments) throws Exception {
        AIAssistantController controller = AIAssistantController.shared(activity);
        File source = controller.workspaceImageForHost(arguments.optString("path", ""));
        File target = controller.workspaceFile(
            "person-mask-" + java.util.UUID.randomUUID() + ".png");
        if (target == null) throw new IllegalStateException("AI 工作区尚未准备好");
        return MobileImageVision.segmentPerson(source, target);
    }

    private JSONObject agentCopyPhotosToAlbum(JSONObject arguments) throws Exception {
        agentPhotoAccess();
        if (Build.VERSION.SDK_INT < 29)
            throw new IllegalStateException("当前 Android 版本无法安全写入系统相簿");
        String album = arguments.optString("album_name", "").trim();
        JSONArray values = arguments.optJSONArray("photo_ids");
        if (!album.matches("[\\p{L}\\p{N} _-]{1,64}") || values == null
                || values.length() < 1 || values.length() > 10)
            throw new IllegalArgumentException("相簿名或照片列表无效");
        Set<Long> seen = new HashSet<>();
        List<Uri> sources = new ArrayList<>();
        List<String> names = new ArrayList<>();
        List<String> mimeTypes = new ArrayList<>();
        long totalBytes = 0;
        String[] columns = {MediaStore.MediaColumns.DISPLAY_NAME,
            MediaStore.MediaColumns.MIME_TYPE, MediaStore.MediaColumns.SIZE};
        for (int index = 0; index < values.length(); index++) {
            long id;
            try { id = Long.parseLong(values.optString(index, "")); }
            catch (NumberFormatException error) { throw new IllegalArgumentException("无效的照片 ID"); }
            if (id <= 0 || !seen.add(id)) continue;
            Uri source = ContentUris.withAppendedId(agentPhotoCollection(), id);
            try (Cursor cursor = activity.getContentResolver().query(source, columns, null, null, null)) {
                if (cursor == null || !cursor.moveToFirst())
                    throw new IllegalArgumentException("有照片不可访问或不存在");
                String mime = cursor.getString(1);
                long size = cursor.getLong(2);
                if (mime == null || !mime.startsWith("image/") || size < 0 || size > 25 * 1024 * 1024)
                    throw new IllegalArgumentException("仅支持每张不超过 25 MB 的图片");
                totalBytes += size;
                if (totalBytes > 100 * 1024 * 1024)
                    throw new IllegalArgumentException("单次复制总量不能超过 100 MB");
                sources.add(source);
                names.add(cursor.getString(0));
                mimeTypes.add(mime);
            }
        }
        if (sources.isEmpty()) throw new IllegalArgumentException("照片列表为空");
        String destinationPath = Environment.DIRECTORY_PICTURES + "/MaiChat/" + album + "/";
        Uri destination = MediaStore.Images.Media.getContentUri(MediaStore.VOLUME_EXTERNAL_PRIMARY);
        List<Uri> copies = new ArrayList<>();
        try {
            for (int index = 0; index < sources.size(); index++) {
                ContentValues metadata = new ContentValues();
                String originalName = names.get(index);
                String safeName = originalName == null ? "photo.jpg"
                    : originalName.replaceAll("[^\\p{L}\\p{N}._-]", "_");
                if (safeName.length() > 120)
                    safeName = safeName.substring(safeName.length() - 120);
                metadata.put(MediaStore.MediaColumns.DISPLAY_NAME,
                    "maichat-" + java.util.UUID.randomUUID().toString().substring(0, 8)
                        + "-" + safeName);
                metadata.put(MediaStore.MediaColumns.MIME_TYPE, mimeTypes.get(index));
                metadata.put(MediaStore.MediaColumns.RELATIVE_PATH, destinationPath);
                metadata.put(MediaStore.MediaColumns.IS_PENDING, 1);
                Uri copy = activity.getContentResolver().insert(destination, metadata);
                if (copy == null) throw new IOException("无法创建相簿副本");
                copies.add(copy);
                try (InputStream input = activity.getContentResolver().openInputStream(sources.get(index));
                     OutputStream output = activity.getContentResolver().openOutputStream(copy)) {
                    if (input == null || output == null) throw new IOException("无法复制照片");
                    byte[] buffer = new byte[8192];
                    int count;
                    long bytesCopied = 0;
                    while ((count = input.read(buffer)) >= 0) {
                        bytesCopied += count;
                        if (bytesCopied > 25 * 1024 * 1024)
                            throw new IOException("照片实际大小超过 25 MB");
                        output.write(buffer, 0, count);
                    }
                }
                ContentValues visible = new ContentValues();
                visible.put(MediaStore.MediaColumns.IS_PENDING, 0);
                if (activity.getContentResolver().update(copy, visible, null, null) < 1)
                    throw new IOException("无法完成相簿副本");
            }
        } catch (Exception error) {
            for (Uri copy : copies) {
                try { activity.getContentResolver().delete(copy, null, null); }
                catch (Exception ignored) { /* Best effort rollback of app-owned copies. */ }
            }
            throw error;
        }
        return new JSONObject().put("album_name", album).put("relative_path", destinationPath)
            .put("copied_count", copies.size()).put("copied_originals", true);
    }

    private static Uri agentPhotoCollection() {
        return Build.VERSION.SDK_INT >= 29
            ? MediaStore.Images.Media.getContentUri(MediaStore.VOLUME_EXTERNAL)
            : MediaStore.Images.Media.EXTERNAL_CONTENT_URI;
    }

    private static Uri agentVideoCollection() {
        return Build.VERSION.SDK_INT >= 29
            ? MediaStore.Video.Media.getContentUri(MediaStore.VOLUME_EXTERNAL)
            : MediaStore.Video.Media.EXTERNAL_CONTENT_URI;
    }

    private static Uri agentVisualCollection() {
        return MediaStore.Files.getContentUri(Build.VERSION.SDK_INT >= 29
            ? MediaStore.VOLUME_EXTERNAL : "external");
    }

    private int agentMediaType(long id) {
        String[] projection = {MediaStore.Files.FileColumns.MEDIA_TYPE};
        try (Cursor cursor = activity.getContentResolver().query(
                ContentUris.withAppendedId(agentVisualCollection(), id),
                projection, null, null, null)) {
            if (cursor != null && cursor.moveToFirst()) {
                int type = cursor.getInt(0);
                if (type == MediaStore.Files.FileColumns.MEDIA_TYPE_IMAGE ||
                    type == MediaStore.Files.FileColumns.MEDIA_TYPE_VIDEO) return type;
            }
        }
        throw new IllegalArgumentException("相册媒体不存在或没有访问权限");
    }

}
