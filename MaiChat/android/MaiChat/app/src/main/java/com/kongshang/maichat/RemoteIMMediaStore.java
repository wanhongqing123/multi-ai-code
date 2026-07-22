package com.kongshang.maichat;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.Locale;
import java.util.function.LongSupplier;

public final class RemoteIMMediaStore {
    private static final String IMAGE_DIRECTORY_NAME = "RemoteIMPickedImage";
    private static final String VOICE_DIRECTORY_NAME = "RemoteIMVoice";

    private final File cacheRoot;
    private final LongSupplier timestampProvider;

    public RemoteIMMediaStore(File cacheRoot) {
        this(cacheRoot, System::currentTimeMillis);
    }

    RemoteIMMediaStore(File cacheRoot, LongSupplier timestampProvider) {
        this.cacheRoot = cacheRoot;
        this.timestampProvider = timestampProvider;
    }

    public File copyPickedImage(InputStream input, String sourceName) throws IOException {
        if (input == null) throw new IOException("image input is empty");
        File directory = ensureDirectory(IMAGE_DIRECTORY_NAME);
        File file = new File(
            directory,
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
        File directory = ensureDirectory(VOICE_DIRECTORY_NAME);
        return new File(directory, "remote-im-voice-" + timestampProvider.getAsLong() + ".m4a");
    }

    private File ensureDirectory(String directoryName) throws IOException {
        File directory = new File(cacheRoot, directoryName);
        if (!directory.exists() && !directory.mkdirs()) {
            throw new IOException("failed to create media cache directory");
        }
        return directory;
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
