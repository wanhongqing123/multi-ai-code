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
    @Test
    public void copiesPickedImageIntoImageCacheDirectory() throws Exception {
        Path root = Files.createTempDirectory("maichat-android-media-image");
        RemoteIMMediaStore mediaStore = new RemoteIMMediaStore(root.toFile(), () -> 42L);
        byte[] imageBytes = new byte[]{1, 2, 3, 4};

        File copiedFile = mediaStore.copyPickedImage(
            new ByteArrayInputStream(imageBytes),
            "photo.png"
        );

        assertEquals("remote-im-image-42.png", copiedFile.getName());
        assertEquals("RemoteIMPickedImage", copiedFile.getParentFile().getName());
        assertArrayEquals(imageBytes, Files.readAllBytes(copiedFile.toPath()));
    }

    @Test
    public void createsVoiceRecordingFileInVoiceCacheDirectory() throws Exception {
        Path root = Files.createTempDirectory("maichat-android-media-voice");
        RemoteIMMediaStore mediaStore = new RemoteIMMediaStore(root.toFile(), () -> 99L);

        File recordingFile = mediaStore.createVoiceRecordingFile();

        assertEquals("remote-im-voice-99.m4a", recordingFile.getName());
        assertEquals("RemoteIMVoice", recordingFile.getParentFile().getName());
        assertTrue(recordingFile.getParentFile().isDirectory());
    }
}
