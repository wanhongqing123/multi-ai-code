package com.kongshang.maichat;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.Locale;
import java.util.function.LongSupplier;

/**
 * 本端发出的媒体的落盘处：选图、录音、拍照、选文件。
 *
 * 这些文件的路径会进聊天记录并被长期引用，所以一律落在 RemoteIMMediaPaths 的
 * 持久根下，不再放缓存目录——放缓存的话系统清一次，历史里的图就永远打不开了。
 */
public final class RemoteIMMediaStore {
    private final RemoteIMMediaPaths paths;
    private final LongSupplier timestampProvider;

    public RemoteIMMediaStore(RemoteIMMediaPaths paths) {
        this(paths, System::currentTimeMillis);
    }

    RemoteIMMediaStore(RemoteIMMediaPaths paths, LongSupplier timestampProvider) {
        this.paths = paths;
        this.timestampProvider = timestampProvider;
    }

    public File copyPickedImage(InputStream input, String sourceName) throws IOException {
        if (input == null) throw new IOException("image input is empty");
        File file = new File(
            outgoing(RemoteIMMediaPaths.IMAGES),
            "remote-im-image-" + timestampProvider.getAsLong() + imageExtension(sourceName)
        );
        try (InputStream source = input; FileOutputStream output = new FileOutputStream(file)) {
            byte[] buffer = new byte[8192];
            int read;
            while ((read = source.read(buffer)) != -1) {
                output.write(buffer, 0, read);
            }
        }
        return file;
    }

    public File createVoiceRecordingFile() throws IOException {
        return new File(
            outgoing(RemoteIMMediaPaths.VOICES),
            "remote-im-voice-" + timestampProvider.getAsLong() + ".m4a"
        );
    }

    /** 拍照的落点。相机通过 FileProvider 写入，所以必须在 file_paths.xml 覆盖的树里。 */
    public File createCameraPhotoFile() throws IOException {
        return new File(
            outgoing(RemoteIMMediaPaths.IMAGES),
            "camera-" + timestampProvider.getAsLong() + ".jpg"
        );
    }

    public File createOutgoingFile(String sourceName) throws IOException {
        return new File(
            outgoing(RemoteIMMediaPaths.FILES),
            "file-" + timestampProvider.getAsLong() + fileSuffix(sourceName)
        );
    }

    private File outgoing(String kind) throws IOException {
        return paths.directory(RemoteIMMediaPaths.OUTGOING, kind);
    }

    private static String fileSuffix(String sourceName) {
        if (sourceName == null) return ".bin";
        int dotIndex = sourceName.lastIndexOf('.');
        if (dotIndex < 0 || dotIndex == sourceName.length() - 1) return ".bin";
        return sourceName.substring(dotIndex);
    }

    private static String imageExtension(String sourceName) {
        if (sourceName == null) return ".jpg";
        int queryIndex = sourceName.indexOf('?');
        String cleanName = queryIndex >= 0 ? sourceName.substring(0, queryIndex) : sourceName;
        int dotIndex = cleanName.lastIndexOf('.');
        if (dotIndex < 0 || dotIndex == cleanName.length() - 1) return ".jpg";
        String extension = cleanName.substring(dotIndex + 1).toLowerCase(Locale.US);
        if (!extension.matches("[a-z0-9]{1,8}")) return ".jpg";
        return "." + extension;
    }
}
