package com.kongshang.maichat;

import android.content.Context;

import java.io.File;
import java.io.IOException;

/**
 * 媒体文件的落盘位置，以及「入库存相对路径」的换算。
 *
 * 不放缓存目录：系统在空间紧张时会清空 cache，用户在设置里点「清除缓存」同样会清空，
 * 而聊天记录会长期引用这些文件——文件一没，历史里的图片和视频就永远打不开。
 *
 * 入库存相对路径而不是绝对路径：app 数据目录的根并非恒定（多用户/工作资料、应用被
 * 迁到可采用存储都会换根），存绝对路径时换根即全部失效。相对路径读取时按当前根还原。
 *
 * 用 filesDir 而不是 noBackupFilesDir：拍照要走 FileProvider.getUriForFile，而
 * FileProvider 只有 files-path/cache-path 这类标签，没有指向 no_backup 目录的，
 * 放进去 getUriForFile 会直接抛异常。媒体体积大，改由备份规则把这棵树排除掉。
 */
public final class RemoteIMMediaPaths {
    public static final String ROOT_DIRECTORY_NAME = "RemoteIMMedia";

    /** 收到的媒体。与发出的分开，是为了让「同名即同一文件」只需在同一命名空间内成立。 */
    public static final String INCOMING = "Incoming";
    public static final String OUTGOING = "Outgoing";

    public static final String IMAGES = "Images";
    public static final String VIDEOS = "Videos";
    public static final String VIDEO_COVERS = "VideoCovers";
    public static final String VOICES = "Voices";
    public static final String FILES = "Files";

    private final File root;

    /**
     * 全 app 唯一的媒体根。写入方和读取还原方必须拿到同一个根，各自 new 一遍
     * 迟早会分叉——一旦分叉，历史里的相对路径就会被还原到没有文件的地方。
     */
    public static RemoteIMMediaPaths forApp(Context context) {
        return new RemoteIMMediaPaths(
            new File(context.getApplicationContext().getFilesDir(), ROOT_DIRECTORY_NAME));
    }

    RemoteIMMediaPaths(File root) {
        this.root = root;
    }

    public File root() {
        return root;
    }

    /** 取（并按需创建）某个方向、某种类型的目录。 */
    public File directory(String direction, String kind) throws IOException {
        File directory = new File(new File(root, direction), kind);
        if (!directory.isDirectory() && !directory.mkdirs()) {
            throw new IOException("create media directory failed: " + directory);
        }
        return directory;
    }

    /**
     * 入库用：根目录下的文件压成相对路径，分隔符一律 '/'，与设备无关。
     * 不在根目录下的（例如历史遗留的缓存目录绝对路径）原样返回，不做改写。
     */
    public String toStoredPath(String absolutePath) {
        if (absolutePath == null || absolutePath.isEmpty()) return "";
        String rootPrefix = root.getAbsolutePath();
        if (!rootPrefix.endsWith(File.separator)) rootPrefix = rootPrefix + File.separator;
        String candidate = new File(absolutePath).getAbsolutePath();
        if (!candidate.startsWith(rootPrefix)) return absolutePath;
        return candidate.substring(rootPrefix.length()).replace(File.separatorChar, '/');
    }

    /**
     * 读库用：相对路径按当前根还原成绝对路径。
     * 绝对路径直接返回——老记录存的就是绝对路径，它们指向的文件多半已不在，
     * 由调用方按「文件已丢失」处理，这里不擅自改写成别的位置。
     */
    public String toAbsolutePath(String storedPath) {
        if (storedPath == null || storedPath.isEmpty()) return "";
        if (new File(storedPath).isAbsolute()) return storedPath;
        return new File(root, storedPath.replace('/', File.separatorChar)).getAbsolutePath();
    }
}
