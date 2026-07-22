package com.multiaicode.remoteim;

import java.io.File;
import java.util.Objects;

public final class RemoteIMFileAttachment {
    private final String localPath;
    private final String fileName;
    private final String mimeType;
    private final long sizeBytes;

    public RemoteIMFileAttachment(String localPath, String fileName, String mimeType, long sizeBytes) {
        this.localPath = clean(localPath);
        String cleanFileName = clean(fileName);
        this.fileName = cleanFileName.isEmpty() ? new File(this.localPath).getName() : cleanFileName;
        this.mimeType = clean(mimeType);
        this.sizeBytes = sizeBytes;
        if (this.localPath.isEmpty()) {
            throw new IllegalArgumentException("localPath is required");
        }
    }

    public String localPath() {
        return localPath;
    }

    public String fileName() {
        return fileName;
    }

    public String mimeType() {
        return mimeType;
    }

    public long sizeBytes() {
        return sizeBytes;
    }

    private static String clean(String value) {
        return value == null ? "" : value.trim();
    }

    @Override
    public boolean equals(Object other) {
        if (this == other) return true;
        if (!(other instanceof RemoteIMFileAttachment)) return false;
        RemoteIMFileAttachment that = (RemoteIMFileAttachment) other;
        return sizeBytes == that.sizeBytes
            && localPath.equals(that.localPath)
            && fileName.equals(that.fileName)
            && mimeType.equals(that.mimeType);
    }

    @Override
    public int hashCode() {
        return Objects.hash(localPath, fileName, mimeType, sizeBytes);
    }
}
