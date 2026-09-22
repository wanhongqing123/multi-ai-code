package com.kongshang.maichat;

import android.os.Looper;
import android.widget.TextView;
import androidx.test.core.app.ApplicationProvider;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.Shadows;
import org.robolectric.annotation.Config;
import java.util.Collections;
import java.util.concurrent.atomic.AtomicBoolean;
import static org.junit.Assert.*;

@RunWith(RobolectricTestRunner.class)
@Config(sdk = 34)
public class MarkdownRendererTest {
    @Test public void backgroundPreparationHandlesNestedMarkdownAndPreservesCodeEscapes() throws Exception {
        MarkdownRenderer.initialize(ApplicationProvider.getApplicationContext());
        String source = "# Heading\n\n- **bold** and [site](https://example.com)\n  - child\n\n`literal \\n`\n\n~~removed~~\n\n| A | B |\n|---|---|\n|one|two|";
        AtomicBoolean completed = new AtomicBoolean();
        MarkdownRenderer.prepare(Collections.singletonList(source), () -> true, () -> completed.set(true));
        long deadline = System.nanoTime() + 3_000_000_000L;
        while (!completed.get() && System.nanoTime() < deadline) {
            Shadows.shadowOf(Looper.getMainLooper()).idle(); Thread.sleep(5);
        }
        assertTrue("Preparation must deliver its result", completed.get());
        TextView text = new TextView(ApplicationProvider.getApplicationContext());
        MarkdownRenderer.bind(text, source);
        String rendered = text.getText().toString();
        assertTrue(rendered.contains("Heading")); assertTrue(rendered.contains("child"));
        assertTrue(rendered.contains("literal \\n")); assertFalse(rendered.contains("**bold**"));
        assertFalse(rendered.contains("[site](https://example.com)"));
        assertFalse(rendered.contains("|---|---|"));
    }
}
