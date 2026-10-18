package com.kongshang.maichat;

import static org.junit.Assert.assertArrayEquals;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;

import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;

public final class AndroidFileOperationsTest {
    @Rule public TemporaryFolder temporary = new TemporaryFolder();

    @Test
    public void publishesNewFileAndPreservesExistingTarget() throws Exception {
        Path directory = temporary.newFolder().toPath();
        Path source = directory.resolve("source.bin");
        Path target = directory.resolve("target.bin");
        Files.write(source, new byte[] {1, 2, 3});

        assertEquals(AndroidFileOperations.OK, AndroidFileOperations.publishNewFile(bytes(source), bytes(target)));
        assertFalse(Files.exists(source));
        assertArrayEquals(new byte[] {1, 2, 3}, Files.readAllBytes(target));

        Files.write(source, new byte[] {4, 5, 6});
        assertEquals(AndroidFileOperations.TARGET_EXISTS,
                AndroidFileOperations.publishNewFile(bytes(source), bytes(target)));
        assertArrayEquals(new byte[] {4, 5, 6}, Files.readAllBytes(source));
        assertArrayEquals(new byte[] {1, 2, 3}, Files.readAllBytes(target));
    }

    @Test
    public void rejectsMalformedPathWithoutPublishing() throws Exception {
        Path directory = temporary.newFolder().toPath();
        Path target = directory.resolve("target.bin");
        assertEquals(AndroidFileOperations.INVALID_PATH,
                AndroidFileOperations.publishNewFile(new byte[] {(byte) 0xC3, (byte) 0x28}, bytes(target)));
        assertFalse(Files.exists(target));
    }

    @Test
    public void preservesUnicodePaths() throws Exception {
        Path directory = temporary.newFolder().toPath();
        Path source = directory.resolve("\u4e34\u65f6.bin");
        Path target = directory.resolve("\u6210\u54c1.bin");
        Files.write(source, new byte[] {42});
        assertEquals(AndroidFileOperations.OK, AndroidFileOperations.publishNewFile(bytes(source), bytes(target)));
        assertFalse(Files.exists(source));
        assertArrayEquals(new byte[] {42}, Files.readAllBytes(target));
    }

    @Test
    public void concurrentPublishersInOneProcessCannotOverwrite() throws Exception {
        Path directory = temporary.newFolder().toPath();
        Path first = directory.resolve("first.bin");
        Path second = directory.resolve("second.bin");
        Path target = directory.resolve("target.bin");
        Files.write(first, new byte[] {1});
        Files.write(second, new byte[] {2});
        CountDownLatch start = new CountDownLatch(1);
        ExecutorService workers = Executors.newFixedThreadPool(2);
        try {
            Future<Integer> firstResult = workers.submit(() -> {
                start.await();
                return AndroidFileOperations.publishNewFile(bytes(first), bytes(target));
            });
            Future<Integer> secondResult = workers.submit(() -> {
                start.await();
                return AndroidFileOperations.publishNewFile(bytes(second), bytes(target));
            });
            start.countDown();
            int firstStatus = firstResult.get();
            int secondStatus = secondResult.get();
            assertEquals(AndroidFileOperations.OK, Math.min(firstStatus, secondStatus));
            assertEquals(AndroidFileOperations.TARGET_EXISTS, Math.max(firstStatus, secondStatus));
            byte[] expected = firstStatus == AndroidFileOperations.OK ? new byte[] {1} : new byte[] {2};
            assertArrayEquals(expected, Files.readAllBytes(target));
            assertEquals(1, (Files.exists(first) ? 1 : 0) + (Files.exists(second) ? 1 : 0));
        } finally {
            workers.shutdownNow();
        }
    }

    private static byte[] bytes(Path path) {
        return path.toString().getBytes(StandardCharsets.UTF_8);
    }
}
