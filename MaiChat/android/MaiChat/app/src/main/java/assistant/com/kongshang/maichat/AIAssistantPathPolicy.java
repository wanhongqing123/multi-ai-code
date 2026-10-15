// 文件作用：Android 创作中心的会话、消息和 Agent 交互；AIAssistantPathPolicy 负责输入筛选、权限判断或展示规则，本文件给出其实现。
// 主要入口：resolve。
package com.kongshang.maichat;

import java.io.File;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.LinkOption;
import java.nio.file.Path;

/** Resolves Agent paths against Workspace while allowing files throughout this App container. */
final class AIAssistantPathPolicy {
    private AIAssistantPathPolicy() {}

    static File resolve(File workspace, File appRoot, String path) throws IOException {
        if (path == null || path.trim().isEmpty())
            throw new IllegalArgumentException("需要 App 目录中的文件路径");
        File candidate = new File(path);
        if (!candidate.isAbsolute()) candidate = new File(workspace, path);
        Path target = candidate.toPath().toAbsolutePath().normalize();
        Path existing = target;
        while (!Files.exists(existing, LinkOption.NOFOLLOW_LINKS)) {
            existing = existing.getParent();
            if (existing == null) throw new IOException("无法解析文件路径");
        }
        Path resolved = existing.toRealPath().resolve(existing.relativize(target)).normalize();
        if (!resolved.startsWith(appRoot.toPath().toRealPath()))
            throw new IllegalArgumentException("文件必须位于当前 App 目录内");
        return resolved.toFile();
    }
}
