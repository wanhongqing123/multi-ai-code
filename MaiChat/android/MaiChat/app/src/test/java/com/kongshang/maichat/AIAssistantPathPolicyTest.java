package com.kongshang.maichat;

import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;

import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertTrue;
import static org.junit.Assert.fail;

public class AIAssistantPathPolicyTest {
    @Rule public TemporaryFolder temporary = new TemporaryFolder();

    @Test public void appSiblingsAreReadableAndWritableButOtherAppsAreDenied() throws Exception {
        File app = temporary.newFolder("app");
        File workspace = new File(app, "Workspace");
        File media = new File(app, "media");
        assertTrue(workspace.mkdir());
        assertTrue(media.mkdir());
        File outside = temporary.newFolder("other-app");
        Files.createSymbolicLink(new File(workspace, "escape").toPath(), outside.toPath());
        File target = new File(media, "note.txt");
        File resolved = AIAssistantPathPolicy.resolve(
            workspace, app, "../media/note.txt");
        assertEquals(target.getCanonicalFile(), resolved);
        Files.write(resolved.toPath(), "hello".getBytes(StandardCharsets.UTF_8));
        assertEquals(target.getCanonicalFile(), AIAssistantPathPolicy.resolve(
            workspace, app, target.getPath()));

        try {
            AIAssistantPathPolicy.resolve(workspace, app,
                new File(outside, "private.txt").getPath());
            fail("A different App container must remain inaccessible");
        } catch (IllegalArgumentException expected) {
            assertTrue(expected.getMessage().contains("App"));
        }
        try {
            AIAssistantPathPolicy.resolve(workspace, app, "escape/private.txt");
            fail("A symlink must not escape the App container");
        } catch (IllegalArgumentException expected) {
            assertTrue(expected.getMessage().contains("App"));
        }
    }
}
