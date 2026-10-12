package com.kongshang.maichat;

import java.io.File;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;
import java.util.function.Function;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/** 只从一条已完成消息中提取可播放文件；超过四个时保留原文件名列表。 */
final class AIAssistantMediaMentions {
    static final int MAX_CARDS = 4;
    private static final Pattern CODE_PATH = Pattern.compile(
        "`([^`\\r\\n]+)`", Pattern.CASE_INSENSITIVE);
    private static final Pattern BARE_PATH = Pattern.compile(
        "[^\\s`()<>\\[\\]{}\"'“”‘’,，。;；:：|]+?\\.(?:mp4|mov|wav|mp3|m4a|aac)(?![A-Za-z0-9_.])",
        Pattern.CASE_INSENSITIVE);

    enum Kind { VIDEO, AUDIO }

    static final class Mention {
        final File file;
        final Kind kind;
        Mention(File file, Kind kind) { this.file = file; this.kind = kind; }
    }

    private AIAssistantMediaMentions() { }

    static List<Mention> cards(String text, Function<String, File> resolver) {
        if (text == null || text.length() > 100_000 || resolver == null)
            return Collections.emptyList();
        List<Mention> result = new ArrayList<>();
        Set<String> seen = new HashSet<>();
        boolean fenced = false;
        for (String line : text.split("\\R", -1)) {
            if (line.trim().startsWith("```")) { fenced = !fenced; continue; }
            if (fenced || line.startsWith("    ")) continue;
            List<int[]> codeRanges = new ArrayList<>();
            Matcher code = CODE_PATH.matcher(line);
            while (code.find()) {
                codeRanges.add(new int[] {code.start(), code.end()});
                add(code.group(1), resolver, seen, result);
                if (result.size() > MAX_CARDS) return Collections.emptyList();
            }
            Matcher bare = BARE_PATH.matcher(line);
            while (bare.find()) {
                boolean insideCode = false;
                for (int[] range : codeRanges)
                    if (bare.start() < range[1] && bare.end() > range[0]) {
                        insideCode = true; break;
                    }
                if (insideCode) continue;
                add(bare.group(), resolver, seen, result);
                if (result.size() > MAX_CARDS) return Collections.emptyList();
            }
        }
        return result;
    }

    private static void add(String raw, Function<String, File> resolver,
                            Set<String> seen, List<Mention> result) {
        String path = raw.trim();
        String lower = path.toLowerCase(Locale.ROOT);
        Kind kind;
        if (lower.endsWith(".mp4") || lower.endsWith(".mov")) kind = Kind.VIDEO;
        else if (lower.endsWith(".wav") || lower.endsWith(".mp3")
            || lower.endsWith(".m4a") || lower.endsWith(".aac")) kind = Kind.AUDIO;
        else return;
        File file = resolver.apply(path);
        if (file != null && file.isFile() && file.length() > 0 && seen.add(file.getPath()))
            result.add(new Mention(file, kind));
    }
}
