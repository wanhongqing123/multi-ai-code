package com.kongshang.maichat;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;
import java.io.OutputStream;

// A test-only provider whose descriptors are pipes, as with remote SAF files.
public final class CloudPipeProvider extends ContentProvider {
    static final int LENGTH = 8 * 1024 * 1024 + 128 * 1024;
    static final Uri URI = Uri.parse(
        "content://" + BuildConfig.APPLICATION_ID + ".test.cloudpipe/video.mp4");

    @Override public boolean onCreate() { return true; }

    @Override public String getType(Uri uri) { return "video/mp4"; }

    @Override public Cursor query(Uri uri, String[] projection, String selection,
                                  String[] selectionArgs, String sortOrder) {
        String[] columns = projection == null ?
            new String[]{OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE} : projection;
        MatrixCursor cursor = new MatrixCursor(columns);
        Object[] values = new Object[columns.length];
        for (int i = 0; i < columns.length; ++i) {
            if (OpenableColumns.DISPLAY_NAME.equals(columns[i])) values[i] = "pipe-video.mp4";
            if (OpenableColumns.SIZE.equals(columns[i])) values[i] = LENGTH;
        }
        cursor.addRow(values);
        return cursor;
    }

    @Override public ParcelFileDescriptor openFile(Uri uri, String mode)
        throws java.io.FileNotFoundException {
        if (!"r".equals(mode) || !URI.equals(uri))
            throw new java.io.FileNotFoundException(uri.toString());
        try {
            ParcelFileDescriptor[] pipe = ParcelFileDescriptor.createPipe();
            new Thread(() -> {
                try (OutputStream output =
                         new ParcelFileDescriptor.AutoCloseOutputStream(pipe[1])) {
                    byte[] chunk = new byte[8192];
                    int written = 0;
                    while (written < LENGTH) {
                        int count = Math.min(chunk.length, LENGTH - written);
                        for (int i = 0; i < count; ++i)
                            chunk[i] = (byte) ((written + i) * 17 + 3);
                        output.write(chunk, 0, count);
                        written += count;
                    }
                } catch (Exception ignored) { }
            }, "cloud-pipe-writer").start();
            return pipe[0];
        } catch (Exception failure) {
            throw new java.io.FileNotFoundException(failure.getMessage());
        }
    }

    @Override public Uri insert(Uri uri, ContentValues values) {
        throw new UnsupportedOperationException();
    }
    @Override public int update(Uri uri, ContentValues values, String selection,
                                String[] selectionArgs) { return 0; }
    @Override public int delete(Uri uri, String selection, String[] selectionArgs) { return 0; }
}
