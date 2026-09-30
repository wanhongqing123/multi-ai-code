package com.kongshang.maichat;

import android.content.Context;

import java.io.File;
import java.io.IOException;
import java.nio.file.Path;

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
     *
     * 不在根目录下的一律返回空串，不入库。库里只承认本 app 自己管理的媒体，
     * 别处的文件不受我们控制、随时可能消失，存进去只会变成又一条悬空引用。
     */
    public String toStoredPath(String absolutePath) {
        if (absolutePath == null || absolutePath.isEmpty()) return "";
        String rootPrefix = root.getAbsolutePath();
        if (!rootPrefix.endsWith(File.separator)) rootPrefix = rootPrefix + File.separator;
        String candidate = new File(absolutePath).getAbsolutePath();
        if (!candidate.startsWith(rootPrefix)) return "";
        return candidate.substring(rootPrefix.length()).replace(File.separatorChar, '/');
    }

    /**
     * 读库用：相对路径按当前根还原成绝对路径。
     *
     * 绝对路径一律返回空串。库里存绝对路径的只有旧版本写下的记录，按用户要求
     * 不做任何旧版本兼容——不读旧路径、不搬运文件，这些记录直接当作没有附件，
     * 由消息正文里的「[图片消息] …」占位文字体现曾经有过附件。
     */
    public String toAbsolutePath(String storedPath) {
        if (storedPath == null || storedPath.isEmpty()) return "";
        if (new File(storedPath).isAbsolute()) return "";
        Path rootPath = root.toPath().toAbsolutePath().normalize();
        // 还原结果必须仍在媒体根之内。库里正常不会出现带 .. 的值，但拼接本身能逃出根，
        // 所以这里自己兜住，而不是指望写入端永远干净。
        Path resolved = rootPath.resolve(storedPath.replace('/', File.separatorChar)).normalize();
        if (!resolved.startsWith(rootPath)) return "";
        return resolved.toString();
    }
}
