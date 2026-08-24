package com.kongshang.maichat;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertTrue;

import org.junit.Test;

import java.io.File;
import java.nio.file.Files;
import java.nio.file.Path;

public class RemoteIMMediaPathsTest {
    @Test
    public void storesPathUnderRootAsSlashSeparatedRelativePath() throws Exception {
        Path root = Files.createTempDirectory("maichat-media-root");
        RemoteIMMediaPaths paths = new RemoteIMMediaPaths(root.toFile());

        File target = new File(
            paths.directory(RemoteIMMediaPaths.INCOMING, RemoteIMMediaPaths.IMAGES),
            "photo.jpg"
        );

        assertEquals("Incoming/Images/photo.jpg", paths.toStoredPath(target.getAbsolutePath()));
    }

    /** 库里只承认本 app 管理的媒体：根外的文件不受我们控制，存进去只是又一条悬空引用。 */
    @Test
    public void dropsPathOutsideRootInsteadOfStoringIt() throws Exception {
        Path root = Files.createTempDirectory("maichat-media-root-foreign");
        Path elsewhere = Files.createTempDirectory("maichat-media-elsewhere");
        RemoteIMMediaPaths paths = new RemoteIMMediaPaths(root.toFile());

        String outsideRoot = new File(elsewhere.toFile(), "cached.jpg").getAbsolutePath();

        assertEquals("", paths.toStoredPath(outsideRoot));
    }

    @Test
    public void resolvesRelativePathAgainstCurrentRoot() throws Exception {
        Path root = Files.createTempDirectory("maichat-media-resolve");
        RemoteIMMediaPaths paths = new RemoteIMMediaPaths(root.toFile());

        String resolved = paths.toAbsolutePath("Outgoing/Videos/clip.mp4");

        assertEquals(
            new File(root.toFile(), "Outgoing" + File.separator + "Videos" + File.separator + "clip.mp4")
                .getAbsolutePath(),
            resolved
        );
    }

    /**
     * 旧版本写下的绝对路径一律不读：按用户要求不做任何旧版本兼容，既不搬运文件也不
     * 沿用旧路径。这类记录当作没有附件，由正文里的「[图片消息] …」占位文字体现。
     */
    @Test
    public void refusesToReadLegacyAbsolutePath() throws Exception {
        Path root = Files.createTempDirectory("maichat-media-legacy");
        Path cache = Files.createTempDirectory("maichat-media-legacy-cache");
        RemoteIMMediaPaths paths = new RemoteIMMediaPaths(root.toFile());

        String legacy = new File(cache.toFile(), "remote-im-media/image/old.jpg").getAbsolutePath();

        assertEquals("", paths.toAbsolutePath(legacy));
    }

    /**
     * 存相对路径的全部意义所在：数据目录换根之后，同一条记录仍然指向新根下的文件。
     * 换成绝对路径入库，这个用例就会挂。
     */
    @Test
    public void relativePathSurvivesRootChange() throws Exception {
        Path oldRoot = Files.createTempDirectory("maichat-media-old-root");
        Path newRoot = Files.createTempDirectory("maichat-media-new-root");

        RemoteIMMediaPaths before = new RemoteIMMediaPaths(oldRoot.toFile());
        File target = new File(
            before.directory(RemoteIMMediaPaths.OUTGOING, RemoteIMMediaPaths.IMAGES),
            "sent.png"
        );
        String stored = before.toStoredPath(target.getAbsolutePath());

        RemoteIMMediaPaths after = new RemoteIMMediaPaths(newRoot.toFile());
        String resolved = after.toAbsolutePath(stored);

        assertTrue(resolved.startsWith(newRoot.toFile().getAbsolutePath()));
        assertTrue(resolved.endsWith("sent.png"));
    }

    @Test
    public void createsDirectoriesPerDirectionAndKind() throws Exception {
        Path root = Files.createTempDirectory("maichat-media-dirs");
        RemoteIMMediaPaths paths = new RemoteIMMediaPaths(root.toFile());

        File incomingCovers =
            paths.directory(RemoteIMMediaPaths.INCOMING, RemoteIMMediaPaths.VIDEO_COVERS);
        File outgoingFiles =
            paths.directory(RemoteIMMediaPaths.OUTGOING, RemoteIMMediaPaths.FILES);

        assertTrue(incomingCovers.isDirectory());
        assertTrue(outgoingFiles.isDirectory());
        assertEquals("VideoCovers", incomingCovers.getName());
        assertEquals("Incoming", incomingCovers.getParentFile().getName());
        assertEquals("Files", outgoingFiles.getName());
        assertEquals("Outgoing", outgoingFiles.getParentFile().getName());
    }

    /** 带 .. 的相对路径拼出来能逃出媒体根，还原时必须挡住。 */
    @Test
    public void refusesRelativePathEscapingTheRoot() throws Exception {
        Path root = Files.createTempDirectory("maichat-media-escape");
        RemoteIMMediaPaths paths = new RemoteIMMediaPaths(root.toFile());

        assertEquals("", paths.toAbsolutePath("../outside.jpg"));
        assertEquals("", paths.toAbsolutePath("Incoming/../../outside.jpg"));
        assertTrue(paths.toAbsolutePath("Incoming/Images/ok.jpg")
            .startsWith(root.toFile().getAbsolutePath()));
    }

    @Test
    public void treatsEmptyPathAsEmpty() throws Exception {
        Path root = Files.createTempDirectory("maichat-media-empty");
        RemoteIMMediaPaths paths = new RemoteIMMediaPaths(root.toFile());

        assertEquals("", paths.toStoredPath(""));
        assertEquals("", paths.toAbsolutePath(""));
        assertEquals("", paths.toStoredPath(null));
        assertEquals("", paths.toAbsolutePath(null));
    }
}
