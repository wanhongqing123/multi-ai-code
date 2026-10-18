package com.kongshang.maichat;

import androidx.annotation.Keep;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.charset.CharacterCodingException;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.nio.file.AccessDeniedException;
import java.nio.file.FileAlreadyExistsException;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.NoSuchFileException;
import java.nio.file.Path;
import java.nio.file.Paths;

/** Android file publication for MaiAgent's native file-system interface. */
@Keep
final class AndroidFileOperations {
    static final int OK = 0;
    static final int TARGET_EXISTS = 1;
    static final int NOT_FOUND = 2;
    static final int ACCESS_DENIED = 3;
    static final int INVALID_PATH = 4;
    static final int IO_ERROR = 5;
    static volatile int publishCallsForTest;

    private AndroidFileOperations() {}

    // Native workers call this method through JNI. Serializing publication also prevents two
    // MaiAgent workers in this process from racing between the Android provider's check and move.
    static synchronized int publishNewFile(byte[] sourceBytes, byte[] targetBytes) {
        if (BuildConfig.DEBUG)
            publishCallsForTest++;
        final Path source;
        final Path target;
        try {
            source = decodePath(sourceBytes);
            target = decodePath(targetBytes);
        } catch (CharacterCodingException | IllegalArgumentException error) {
            return INVALID_PATH;
        }
        try {
            if (Files.exists(target, LinkOption.NOFOLLOW_LINKS))
                return TARGET_EXISTS;
            // Android's default move refuses an existing target. Both paths are in the same
            // App-private workspace, so its provider performs a rename instead of copy/delete.
            Files.move(source, target);
            return OK;
        } catch (FileAlreadyExistsException error) {
            return TARGET_EXISTS;
        } catch (NoSuchFileException error) {
            return NOT_FOUND;
        } catch (AccessDeniedException | SecurityException error) {
            return ACCESS_DENIED;
        } catch (IOException error) {
            return IO_ERROR;
        }
    }

    private static Path decodePath(byte[] bytes) throws CharacterCodingException {
        if (bytes == null || bytes.length == 0)
            throw new IllegalArgumentException("Empty path");
        String value = StandardCharsets.UTF_8.newDecoder()
                               .onMalformedInput(CodingErrorAction.REPORT)
                               .onUnmappableCharacter(CodingErrorAction.REPORT)
                               .decode(ByteBuffer.wrap(bytes))
                               .toString();
        return Paths.get(value);
    }
}
