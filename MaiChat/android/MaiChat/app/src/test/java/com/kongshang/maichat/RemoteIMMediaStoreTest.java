package com.kongshang.maichat;

import static org.junit.Assert.assertArrayEquals;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertTrue;

import org.junit.Test;

import java.io.ByteArrayInputStream;
import java.io.File;
import java.nio.file.Files;
import java.nio.file.Path;

public class RemoteIMMediaStoreTest {
    private static RemoteIMMediaStore store(Path root, long timestamp) {
        return new RemoteIMMediaStore(new RemoteIMMediaPaths(root.toFile()), () -> timestamp);
    }

    @Test
    public void copiesPickedImageIntoOutgoingImagesDirectory() throws Exception {
        Path root = Files.createTempDirectory("maichat-android-media-image");
        byte[] imageBytes = new byte[]{1, 2, 3, 4};

        File copiedFile = store(root, 42L).copyPickedImage(
            new ByteArrayInputStream(imageBytes),
            "photo.png"
        );

        assertEquals("remote-im-image-42.png", copiedFile.getName());
        assertEquals("Images", copiedFile.getParentFile().getName());
        assertEquals("Outgoing", copiedFile.getParentFile().getParentFile().getName());
        assertArrayEquals(imageBytes, Files.readAllBytes(copiedFile.toPath()));
    }

    @Test
    public void createsVoiceRecordingFileInOutgoingVoicesDirectory() throws Exception {
        Path root = Files.createTempDirectory("maichat-android-media-voice");

        File recordingFile = store(root, 99L).createVoiceRecordingFile();

        assertEquals("remote-im-voice-99.m4a", recordingFile.getName());
        assertEquals("Voices", recordingFile.getParentFile().getName());
        assertEquals("Outgoing", recordingFile.getParentFile().getParentFile().getName());
        assertTrue(recordingFile.getParentFile().isDirectory());
    }

    @Test
    public void createsCameraPhotoFileInOutgoingImagesDirectory() throws Exception {
        Path root = Files.createTempDirectory("maichat-android-media-camera");

        File photoFile = store(root, 7L).createCameraPhotoFile();

        assertEquals("camera-7.jpg", photoFile.getName());
        assertEquals("Images", photoFile.getParentFile().getName());
        assertEquals("Outgoing", photoFile.getParentFile().getParentFile().getName());
        assertTrue(photoFile.getParentFile().isDirectory());
    }

    @Test
    public void createsOutgoingFileKeepingSourceSuffix() throws Exception {
        Path root = Files.createTempDirectory("maichat-android-media-file");

        File target = store(root, 5L).createOutgoingFile("report.pdf");

        assertEquals("file-5.pdf", target.getName());
        assertEquals("Files", target.getParentFile().getName());
        assertEquals("Outgoing", target.getParentFile().getParentFile().getName());
    }

    @Test
    public void fallsBackToBinarySuffixWhenSourceHasNoExtension() throws Exception {
        Path root = Files.createTempDirectory("maichat-android-media-file-noext");

        File target = store(root, 6L).createOutgoingFile("no-extension-here");

        assertEquals("file-6.bin", target.getName());
    }

    /**
     * 所有产物都必须落在传入的根之下，不能有任何一条溜到根外面去。
     *
     * 注意这条测不到「根本身是不是缓存目录」——根是外部传入的，选 filesDir 还是
     * cacheDir 发生在 MainActivity/TencentIMClient 里，那里需要 Context，JVM 单测
     * 覆盖不到，只能靠代码审查。这里只锁住本类的不变式。
     */
    @Test
    public void keepsEveryOutgoingFileUnderThePersistentRoot() throws Exception {
        Path root = Files.createTempDirectory("maichat-android-media-root-check");
        RemoteIMMediaStore mediaStore = store(root, 1L);
        String rootPath = root.toFile().getAbsolutePath();

        File[] produced = new File[]{
            mediaStore.copyPickedImage(new ByteArrayInputStream(new byte[]{9}), "a.jpg"),
            mediaStore.createVoiceRecordingFile(),
            mediaStore.createCameraPhotoFile(),
            mediaStore.createOutgoingFile("b.txt")
        };

        for (File file : produced) {
            assertTrue(
                file.getAbsolutePath() + " 不在持久根下",
                file.getAbsolutePath().startsWith(rootPath)
            );
        }
    }
}
