package com.kongshang.maichat;

import android.graphics.Color;
import android.graphics.Typeface;
import android.text.Spannable;
import android.text.SpannableStringBuilder;
import android.text.style.BackgroundColorSpan;
import android.text.style.ForegroundColorSpan;
import android.text.style.RelativeSizeSpan;
import android.text.style.StyleSpan;
import android.text.style.TypefaceSpan;

public final class MarkdownRenderer {
    private MarkdownRenderer() {
    }

    public static CharSequence render(String source) {
        String normalized = source == null ? "" : source.replace("\\n", "\n");
        SpannableStringBuilder output = new SpannableStringBuilder();
        boolean inCode = false;
        String[] lines = normalized.split("\n", -1);
        for (int lineIndex = 0; lineIndex < lines.length; lineIndex += 1) {
            String line = lines[lineIndex];
            if (line.trim().startsWith("```")) {
                inCode = !inCode;
                continue;
            }
            int start = output.length();
            if (inCode) {
                output.append(line);
                int end = output.length();
                output.setSpan(new TypefaceSpan("monospace"), start, end, Spannable.SPAN_EXCLUSIVE_EXCLUSIVE);
                output.setSpan(new BackgroundColorSpan(Color.rgb(241, 245, 249)), start, end, Spannable.SPAN_EXCLUSIVE_EXCLUSIVE);
            } else {
                int headingLevel = headingLevel(line);
                String content = headingLevel > 0 ? line.substring(headingLevel).trim() : line;
                if (content.startsWith("- ") || content.startsWith("* ")) {
                    output.append("• ");
                    content = content.substring(2);
                } else if (content.matches("^[0-9]+\\.\\s+.*")) {
                    int separator = content.indexOf(' ');
                    output.append(content, 0, separator + 1);
                    content = content.substring(separator + 1);
                } else if (content.startsWith("> ")) {
                    output.append("▌ ");
                    content = content.substring(2);
                }
                appendInline(output, content);
                int end = output.length();
                if (headingLevel > 0) {
                    output.setSpan(new StyleSpan(Typeface.BOLD), start, end, Spannable.SPAN_EXCLUSIVE_EXCLUSIVE);
                    output.setSpan(
                        new RelativeSizeSpan(Math.max(1.05f, 1.34f - headingLevel * 0.06f)),
                        start,
                        end,
                        Spannable.SPAN_EXCLUSIVE_EXCLUSIVE
                    );
                }
                if (line.startsWith("> ")) {
                    output.setSpan(
                        new ForegroundColorSpan(MaiChatTheme.SECONDARY),
                        start,
                        end,
                        Spannable.SPAN_EXCLUSIVE_EXCLUSIVE
                    );
                }
            }
            if (lineIndex < lines.length - 1) output.append('\n');
        }
        return output;
    }

    private static void appendInline(SpannableStringBuilder output, String line) {
        int index = 0;
        while (index < line.length()) {
            if (line.startsWith("**", index)) {
                int endMarker = line.indexOf("**", index + 2);
                if (endMarker > index + 2) {
                    int start = output.length();
                    output.append(line, index + 2, endMarker);
                    output.setSpan(
                        new StyleSpan(Typeface.BOLD),
                        start,
                        output.length(),
                        Spannable.SPAN_EXCLUSIVE_EXCLUSIVE
                    );
                    index = endMarker + 2;
                    continue;
                }
            }
            if (line.charAt(index) == '`') {
                int endMarker = line.indexOf('`', index + 1);
                if (endMarker > index + 1) {
                    int start = output.length();
                    output.append(line, index + 1, endMarker);
                    int end = output.length();
                    output.setSpan(new TypefaceSpan("monospace"), start, end, Spannable.SPAN_EXCLUSIVE_EXCLUSIVE);
                    output.setSpan(new BackgroundColorSpan(Color.rgb(226, 232, 240)), start, end, Spannable.SPAN_EXCLUSIVE_EXCLUSIVE);
                    index = endMarker + 1;
                    continue;
                }
            }
            output.append(line.charAt(index));
            index += 1;
        }
    }

    private static int headingLevel(String line) {
        int level = 0;
        while (level < line.length() && level < 6 && line.charAt(level) == '#') level += 1;
        return level > 0 && level < line.length() && line.charAt(level) == ' ' ? level : 0;
    }
}
