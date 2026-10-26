// Android ffplay lifecycle adapter for private cloud video cache files.
package com.kongshang.maichat;

import android.os.StatFs;
import java.io.File;

final class MaiCloudDrivePlaybackCache {
    private static final long RESERVE_BYTES = 64L * 1024 * 1024;

    private MaiCloudDrivePlaybackCache() { }

    static boolean hasSpace(File directory, long videoBytes) {
        return hasSpace(new StatFs(directory.getAbsolutePath()).getAvailableBytes(), videoBytes);
    }

    static boolean hasSpace(long availableBytes, long videoBytes) {
        return videoBytes > 0 && videoBytes <= MaiCloudDrive.MAX_VIDEO_BYTES &&
            availableBytes >= videoBytes + RESERVE_BYTES;
    }

    static void cleanupIdle(File directory) {
        File[] files = directory.listFiles();
        if (files == null) return;
        MaiFfplayVideoView active = MaiFfplayVideoView.activeView();
        String currentPath = active == null ? "" : active.currentVideoPath();
        for (File file : files) {
            if (file.isFile() && !file.getAbsolutePath().equals(currentPath) &&
                (file.getName().endsWith(".mp4") || file.getName().endsWith(".mov")))
                file.delete();
        }
    }

}
