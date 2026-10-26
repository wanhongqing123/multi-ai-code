// Agent adapter for the account-private video drive. All transport, multipart
// state and durable checkpoints remain in the shared C++ MaiCloudDriveClient.
package com.kongshang.maichat;

import android.net.Uri;
import android.content.Context;
import android.os.Looper;
import android.os.Handler;
import android.os.SystemClock;
import java.io.File;
import java.io.IOException;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;
import java.util.UUID;
import java.util.Locale;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.function.Consumer;
import java.util.regex.Pattern;
import org.json.JSONArray;
import org.json.JSONObject;

/** Thin worker-thread bridge from cloud Agent tools to the regular cloud-drive client. */
public final class MaiCloudDriveAgentTools {
    public static final class Failure extends Exception {
        public final String code;
        Failure(String code, String message) { super(message); this.code = code; }
    }

    private static final class ActiveUpload {
        final String account;
        final String name;
        final long bytes;
        final MaiCloudDrive client;
        volatile boolean pausing;
        volatile boolean settled;
        ActiveUpload(String account, MaiCloudDrive.Source source, MaiCloudDrive client) {
            this.account = account;
            this.name = source.name;
            this.bytes = source.bytes;
            this.client = client;
        }
    }

    private static final Map<String, ActiveUpload> active = new ConcurrentHashMap<>();
    private static final Object taskLock = new Object();
    private static final Set<String> aborting = new HashSet<>();
    private static final Pattern TASK_ID = Pattern.compile(
        "[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}");

    private static final class CheckpointTask {
        final File checkpoint;
        final String sourceId;
        final JSONObject record;
        CheckpointTask(File checkpoint, JSONObject record) {
            this.checkpoint = checkpoint;
            this.sourceId = record.optString("source_id", "");
            this.record = record;
        }
    }

    private MaiCloudDriveAgentTools() { }

    public static void cancelAll(Context context) {
        for (ActiveUpload upload : active.values()) {
            upload.pausing = true;
            upload.client.cancel();
        }
        // The worker-side settled callback removes each task after its native
        // call returns. Clearing here would allow a fast logout/login cycle to
        // resume the same checkpoint while the prior upload still owns it.
        if (context != null) {
            File root = new File(context.getCacheDir(), "cloud-drive-agent-playback");
            CountDownLatch stopped = new CountDownLatch(1);
            Runnable close = () -> {
                MaiFfplayVideoView view = MaiFfplayVideoView.activeView();
                if (view != null) {
                    boolean closed = context instanceof MainActivity &&
                        ((MainActivity) context).commandAgentVideo("close", 0, 0, 0);
                    if (!closed) view.stop();
                }
                stopped.countDown();
            };
            if (Looper.myLooper() == Looper.getMainLooper()) close.run();
            else new Handler(Looper.getMainLooper()).post(close);
            boolean safeToDelete = false;
            try { safeToDelete = stopped.await(5, TimeUnit.SECONDS); }
            catch (InterruptedException interrupted) { Thread.currentThread().interrupt(); }
            File[] accounts = root.listFiles();
            if (accounts != null) for (File directory : accounts) {
                if (safeToDelete) MaiCloudDrivePlaybackCache.cleanupIdle(directory);
                directory.delete();
            }
            root.delete();
        }
    }

    public static JSONObject execute(MainActivity activity, String tool, JSONObject arguments)
        throws Exception {
        if (Looper.myLooper() == Looper.getMainLooper())
            throw new Failure("internal", "cloud drive Agent tools require a worker thread");
        final String account = MaiChatAuthClient.shared(activity).accountId();
        if (account.isEmpty())
            throw new Failure("not_configured", "MaiChat account login is required");
        MaiCloudDrive client = new MaiCloudDrive(activity);
        switch (tool) {
        case "maichat_cloud_drive_list_media":
            return list(activity, client, account, arguments);
        case "maichat_cloud_drive_list_pending_uploads":
            return pending(activity, client, account);
        case "maichat_cloud_drive_upload_media":
            gate(activity, client, account);
            return upload(client, account,
                workspaceMedia(activity, arguments.optString("file_path", "")), null);
        case "maichat_cloud_drive_pause_upload":
            return pause(activity, client, account, arguments.optString("task_id", ""));
        case "maichat_cloud_drive_resume_upload":
            gate(activity, client, account);
            return resume(activity, client, account, arguments.optString("task_id", ""));
        case "maichat_cloud_drive_abort_upload":
            return abort(activity, client, account, arguments.optString("task_id", ""));
        case "maichat_cloud_drive_open_media":
            gate(activity, client, account);
            return play(activity, client, account, arguments.optString("media_id", ""));
        case "maichat_cloud_drive_delete_media":
            gate(activity, client, account);
            return delete(activity, client, account, arguments.optString("media_id", ""));
        default:
            throw new Failure("invalid_input", "unknown cloud drive tool");
        }
    }

    private interface StartRequest {
        void start(Consumer<MaiCloudDrive.Result> completion);
    }

    private static MaiCloudDrive.Result await(MainActivity activity, MaiCloudDrive client,
                                               String account, StartRequest request,
                                               int timeoutSeconds) throws Failure {
        CompletableFuture<MaiCloudDrive.Result> completed = new CompletableFuture<>();
        try { request.start(completed::complete); }
        catch (RuntimeException failure) {
            throw new Failure("network", "cloud drive operation could not start");
        }
        long deadline = SystemClock.elapsedRealtime() + timeoutSeconds * 1000L;
        while (true) {
            if (!account.equals(MaiChatAuthClient.shared(activity).accountId())) {
                client.cancel();
                throw new Failure("not_configured", "MaiChat account changed");
            }
            long remaining = deadline - SystemClock.elapsedRealtime();
            if (remaining <= 0) {
                client.cancel();
                throw new Failure("network", "cloud drive operation timed out");
            }
            try { return completed.get(Math.min(remaining, 200), TimeUnit.MILLISECONDS); }
            catch (TimeoutException retry) { /* Check account and cancellation again. */ }
            catch (InterruptedException stopped) {
                client.cancel();
                Thread.currentThread().interrupt();
                throw new Failure("canceled", "cloud drive operation was interrupted");
            } catch (ExecutionException failure) {
                throw new Failure("network", "cloud drive operation failed");
            }
        }
    }

    private static Failure error(MaiCloudDrive.Result result, boolean gate) {
        if (result.httpStatus == 401 || result.httpStatus == 503 ||
            (gate && result.httpStatus == 404))
            return new Failure("not_configured", "private cloud drive is unavailable");
        if (result.httpStatus == 404 || result.httpStatus == 410)
            return new Failure("not_found", "cloud media or upload was not found");
        if (result.httpStatus == 413)
            return new Failure("invalid_input", "media exceeds its per-file limit or account quota is full");
        if (result.status == MaiCloudDrive.CANCELED)
            return new Failure("canceled", "cloud drive operation was canceled");
        return new Failure("network", "cloud drive operation failed");
    }

    private static JSONObject list(MainActivity activity, MaiCloudDrive client,
                                   String account, JSONObject arguments)
        throws Failure {
        MaiCloudDrive.Result result = awaitClient(activity, client, account,
            callback -> client.request(MaiCloudDrive.LIST, arguments, callback));
        if (!result.ok()) throw error(result, true);
        JSONObject body = result.json();
        JSONArray videos = body.optJSONArray("videos");
        JSONArray media = new JSONArray();
        if (videos != null) for (int i = 0; i < videos.length(); ++i) {
            JSONObject item = videos.optJSONObject(i);
            if (item == null || item.optString("id", "").isEmpty())
                throw new Failure("protocol", "cloud media list contains an invalid ID");
            try {
                item.put("media_id", item.optString("id"));
                item.remove("id");
                if (!item.has("media_type")) item.put("media_type",
                    item.optString("content_type", "").startsWith("image/") ? "image" : "video");
                media.put(item);
            } catch (Exception invalid) {
                throw new Failure("protocol", "cloud media list is invalid");
            }
        }
        body.remove("videos");
        try { body.put("media", media); }
        catch (Exception impossible) { throw new Failure("internal", "could not encode cloud media list"); }
        return body;
    }

    private static MaiCloudDrive.Result awaitClient(MainActivity activity,
                                                     MaiCloudDrive client, String account,
                                                     StartRequest request) throws Failure {
        return await(activity, client, account, request, 45);
    }

    private static void gate(MainActivity activity, MaiCloudDrive client,
                             String account) throws Failure {
        MaiCloudDrive.Result result = awaitClient(activity, client, account,
            callback -> client.request(MaiCloudDrive.LIST, new JSONObject(), callback));
        if (!result.ok()) throw error(result, true);
    }

    private static JSONObject pending(MainActivity activity, MaiCloudDrive client,
                                      String account) throws Failure {
        MaiCloudDrive.Result result = awaitClient(activity, client, account,
            client::checkpoints);
        if (!result.ok()) throw error(result, false);
        JSONArray items = new JSONArray();
        Map<String, Boolean> seen = new HashMap<>();
        JSONArray uploads = result.json().optJSONArray("uploads");
        if (uploads != null) for (int i = 0; i < uploads.length(); ++i) {
            JSONObject record = uploads.optJSONObject(i);
            if (record == null) continue;
            String file = new File(record.optString("checkpoint_path", "")).getName();
            if (!file.endsWith(".maicloud.json")) continue;
            String id = file.substring(0, file.length() - ".maicloud.json".length());
            if (!TASK_ID.matcher(id).matches()) continue;
            try {
                ActiveUpload activeTask = active.get(account + ":" + id);
                items.put(new JSONObject().put("task_id", id)
                    .put("name", record.optString("name", ""))
                    .put("size_bytes", record.optLong("size_bytes", 0))
                    .put("state", activeTask == null ? "paused" :
                        activeTask.pausing ? "pausing" : "uploading"));
                seen.put(id, true);
            } catch (Exception ignored) { }
        }
        for (Map.Entry<String, ActiveUpload> entry : active.entrySet()) {
            ActiveUpload task = entry.getValue();
            String prefix = account + ":";
            if (!entry.getKey().startsWith(prefix)) continue;
            String id = entry.getKey().substring(prefix.length());
            if (seen.containsKey(id)) continue;
            try { items.put(new JSONObject().put("task_id", id).put("name", task.name)
                .put("size_bytes", task.bytes)
                .put("state", task.pausing ? "pausing" : "uploading")); }
            catch (Exception ignored) { }
        }
        try { return new JSONObject().put("uploads", items); }
        catch (Exception impossible) { throw new Failure("internal", "could not encode upload list"); }
    }

    private static File workspaceMedia(MainActivity activity, String path) throws Failure {
        try { return AIAssistantController.shared(activity).workspaceMediaForHost(path); }
        catch (Exception invalid) {
            throw new Failure("invalid_input", "media must be inside the Agent workspace");
        }
    }

    private static JSONObject upload(MaiCloudDrive client, String account,
                                     File file, File checkpoint) throws Exception {
        MaiCloudDrive.Source source;
        try { source = client.inspect(file); }
        catch (IOException invalid) {
            throw new Failure("invalid_input", "media must be readable and within its size limit");
        }
        return startUpload(client, account, source, checkpoint == null ?
            client.newCheckpoint() : checkpoint);
    }

    private static JSONObject startUpload(MaiCloudDrive client, String account,
                                          MaiCloudDrive.Source source, File checkpoint)
        throws Exception {
        String filename = checkpoint.getName();
        if (!filename.endsWith(".maicloud.json"))
            throw new Failure("invalid_input", "invalid private upload task");
        String id = filename.substring(0, filename.length() - ".maicloud.json".length());
        if (!TASK_ID.matcher(id).matches())
            throw new Failure("invalid_input", "task_id must be a UUID");
        String key = account + ":" + id;
        ActiveUpload task = new ActiveUpload(account, source, client);
        synchronized (taskLock) {
            if (aborting.contains(key))
                throw new Failure("busy", "upload is being discarded");
            if (active.putIfAbsent(key, task) != null)
                throw new Failure("busy", "upload is already running");
        }
        try {
            client.upload(source, checkpoint, (done, total, preparing) -> { },
                result -> { }, () -> {
                    task.settled = true;
                    active.remove(key, task);
                });
            return new JSONObject().put("accepted", true).put("task_id", id)
                .put("state", "uploading");
        } catch (Exception failure) {
            active.remove(key, task);
            throw failure;
        }
    }

    private static JSONObject resume(MainActivity activity, MaiCloudDrive client,
                                     String account, String id)
        throws Exception {
        CheckpointTask task = findCheckpoint(activity, client, account, id);
        File checkpoint = task.checkpoint;
        JSONObject record = task.record;
        String sourceId = task.sourceId;
        MaiCloudDrive.Source source;
        if (sourceId.startsWith("content://")) {
            try { source = client.inspect(Uri.parse(sourceId)); }
            catch (IOException unavailable) {
                throw new Failure("not_found", "original upload media permission is unavailable");
            }
        } else {
            File file = new File(sourceId).getCanonicalFile();
            File managedStage = new File(checkpoint.getAbsolutePath() + ".source")
                .getCanonicalFile();
            if (!file.equals(managedStage)) file = workspaceMedia(activity, sourceId);
            long bytes = record.optLong("size_bytes", -1);
            if (!file.isFile() || file.length() != bytes)
                throw new Failure("not_found", "original upload media is unavailable");
            source = new MaiCloudDrive.Source(null, file,
                record.optString("name", ""),
                record.optString("content_type", ""), bytes);
        }
        return startUpload(client, account, source, checkpoint);
    }

    private static String taskId(String id) throws Failure {
        if (id == null || !TASK_ID.matcher(id).matches())
            throw new Failure("invalid_input", "task_id must be a UUID");
        return id.toLowerCase(Locale.ROOT);
    }

    private static CheckpointTask findCheckpoint(MainActivity activity,
                                                 MaiCloudDrive client,
                                                 String account, String id)
        throws Exception {
        id = taskId(id);
        MaiCloudDrive.Result listed = awaitClient(activity, client, account,
            client::checkpoints);
        if (!listed.ok()) throw error(listed, false);
        File directory = client.accountDirectory().getCanonicalFile();
        JSONArray uploads = listed.json().optJSONArray("uploads");
        if (uploads != null) for (int i = 0; i < uploads.length(); ++i) {
            JSONObject record = uploads.optJSONObject(i);
            if (record == null) continue;
            File checkpoint = new File(record.optString("checkpoint_path", ""))
                .getCanonicalFile();
            if (!checkpoint.getName().equals(id + ".maicloud.json") ||
                !directory.equals(checkpoint.getParentFile())) continue;
            return new CheckpointTask(checkpoint, record);
        }
        throw new Failure("not_found", "paused upload task was not found");
    }

    private static JSONObject pause(MainActivity activity, MaiCloudDrive client,
                                    String account, String id) throws Exception {
        id = taskId(id);
        String key = account + ":" + id;
        ActiveUpload upload;
        synchronized (taskLock) {
            if (aborting.contains(key))
                throw new Failure("busy", "upload is being discarded");
            upload = active.get(key);
            if (upload != null) upload.pausing = true;
        }
        if (upload != null) {
            upload.client.cancel();
            return new JSONObject().put("accepted", true).put("task_id", id)
                .put("state", "pausing");
        }
        findCheckpoint(activity, client, account, id);
        return new JSONObject().put("paused", true).put("task_id", id)
            .put("state", "paused");
    }

    private static JSONObject abort(MainActivity activity, MaiCloudDrive client,
                                    String account, String id) throws Exception {
        id = taskId(id);
        String key = account + ":" + id;
        synchronized (taskLock) {
            if (active.containsKey(key))
                throw new Failure("busy", "pause the active upload first");
            if (!aborting.add(key))
                throw new Failure("busy", "upload is already being discarded");
        }
        try {
            gate(activity, client, account);
            CheckpointTask task = findCheckpoint(activity, client, account, id);
            File stage = new File(task.checkpoint.getAbsolutePath() + ".source");
            boolean ownedStage = stage.getCanonicalFile().getParentFile().equals(
                client.accountDirectory().getCanonicalFile()) &&
                stage.getCanonicalPath().equals(new File(task.sourceId).getCanonicalPath());
            MaiCloudDrive.Result result = awaitClient(activity, client, account,
                callback -> client.abort(task.checkpoint, callback));
            if (!result.ok()) throw error(result, false);
            if (task.checkpoint.exists())
                throw new Failure("internal", "cloud upload record could not be removed");
            if (ownedStage && Files.exists(stage.toPath(), LinkOption.NOFOLLOW_LINKS)) {
                if (!Files.isRegularFile(stage.toPath(), LinkOption.NOFOLLOW_LINKS) ||
                    !stage.delete())
                    throw new Failure("internal", "private staged video could not be removed");
            }
            return new JSONObject().put("aborted", true).put("task_id", id);
        } finally {
            synchronized (taskLock) { aborting.remove(key); }
        }
    }

    private static JSONObject play(MainActivity activity, MaiCloudDrive client,
                                   String account, String id) throws Exception {
        if (id.isEmpty()) throw new Failure("invalid_input", "media_id is required");
        JSONObject request = new JSONObject().put("video_id", id);
        MaiCloudDrive.Result result = awaitClient(activity, client, account,
            callback -> client.request(MaiCloudDrive.PLAY_LINK, request, callback));
        if (!result.ok()) throw error(result, false);
        JSONObject playback = result.json();
        String url = playback.optString("read_url", "");
        long sizeBytes = playback.optLong("size_bytes", -1);
        String contentType = playback.optString("content_type", "");
        String suffix;
        switch (contentType) {
        case "video/mp4": suffix = ".mp4"; break;
        case "video/quicktime": suffix = ".mov"; break;
        case "image/jpeg": suffix = ".jpg"; break;
        case "image/png": suffix = ".png"; break;
        case "image/webp": suffix = ".webp"; break;
        case "image/heic": suffix = ".heic"; break;
        default: throw new Failure("protocol", "cloud media type is unsupported");
        }
        boolean image = contentType.startsWith("image/");
        long maximum = image ? 20_000_000L : MaiCloudDrive.MAX_VIDEO_BYTES;
        if (!url.startsWith("https://"))
            throw new Failure("protocol", "cloud media link is invalid");
        if (sizeBytes < 1 || sizeBytes > maximum ||
            !playback.optString("media_type", "")
                .equals(image ? "image" : "video"))
            throw new Failure("protocol", "cloud media size or type is unavailable");
        File directory = image ? new File(activity.getCacheDir(), "cloud-drive-images") :
            new File(new File(activity.getCacheDir(), "cloud-drive-agent-playback"),
                MaiCloudDrive.accountKey(account));
        if (!directory.isDirectory() && !directory.mkdirs())
            throw new Failure("internal", "could not create private playback cache");
        if (!image) MaiCloudDrivePlaybackCache.cleanupIdle(directory);
        if (!MaiCloudDrivePlaybackCache.hasSpace(directory, sizeBytes))
            throw new Failure("invalid_input", "device lacks space for private media preview");
        File target = new File(directory, UUID.randomUUID() + suffix);
        MaiChatAttachmentDownload.Scope scope = new MaiChatAttachmentDownload.Scope();
        CompletableFuture<MaiChatAttachmentDownload.Result> fetched = new CompletableFuture<>();
        AtomicBoolean allowOpen = new AtomicBoolean(true);
        new MaiChatAttachmentDownload.NativeTransport(client::trustPem).download(url, target,
            sizeBytes, scope, fetched::complete);
        try {
            long deadline = SystemClock.elapsedRealtime() + 20 * 60 * 1000L;
            MaiChatAttachmentDownload.Result downloaded;
            while (true) {
                if (!account.equals(MaiChatAuthClient.shared(activity).accountId())) {
                    scope.cancel();
                    throw new Failure("not_configured", "MaiChat account changed");
                }
                long remaining = deadline - SystemClock.elapsedRealtime();
                if (remaining <= 0) {
                    scope.cancel();
                    throw new Failure("network", "cloud media download timed out");
                }
                try {
                    downloaded = fetched.get(Math.min(remaining, 200), TimeUnit.MILLISECONDS);
                    break;
                } catch (TimeoutException retry) { /* Recheck account. */ }
            }
            if (!downloaded.ok()) throw new Failure("network", "cloud media download failed");
            if (!account.equals(MaiChatAuthClient.shared(activity).accountId()))
                throw new Failure("not_configured", "MaiChat account changed");
            CompletableFuture<Boolean> opened = new CompletableFuture<>();
            activity.runOnUiThread(() -> {
                boolean success = allowOpen.get() &&
                    account.equals(MaiChatAuthClient.shared(activity).accountId()) &&
                    (image ? activity.openCloudDriveImage(target.getAbsolutePath()) :
                        activity.openCloudDriveVideo(target.getAbsolutePath()));
                if (!success) target.delete();
                opened.complete(success);
            });
            if (!opened.get(5, TimeUnit.SECONDS))
                throw new Failure("internal", "cloud media viewer could not open");
            return new JSONObject().put("opened", true).put("media_id", id)
                .put("media_type", image ? "image" : "video");
        } catch (InterruptedException canceled) {
            allowOpen.set(false);
            scope.cancel();
            target.delete();
            Thread.currentThread().interrupt();
            throw new Failure("canceled", "cloud media preview was interrupted");
        } catch (ExecutionException | TimeoutException failure) {
            allowOpen.set(false);
            scope.cancel();
            target.delete();
            throw new Failure("internal", "cloud media viewer failed to open");
        } catch (Failure failure) {
            allowOpen.set(false);
            target.delete();
            throw failure;
        }
    }

    private static JSONObject delete(MainActivity activity, MaiCloudDrive client,
                                     String account, String id)
        throws Exception {
        if (id.isEmpty()) throw new Failure("invalid_input", "media_id is required");
        JSONObject request = new JSONObject().put("video_id", id);
        MaiCloudDrive.Result result = awaitClient(activity, client, account,
            callback -> client.request(MaiCloudDrive.DELETE, request, callback));
        if (!result.ok()) throw error(result, false);
        return new JSONObject().put("deleted", true).put("media_id", id)
            .put("cleanup_pending", result.json().optBoolean("cleanup_pending", false));
    }
}
