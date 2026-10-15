// 文件作用：Android IM 身份、消息同步与本地历史；RemoteIMSlashCommand 负责斜杠命令的数据结构与参数，本文件给出其实现。
// 主要入口：command、label。
package com.kongshang.maichat;

public final class RemoteIMSlashCommand {
    private final String command;
    private final String label;

    public RemoteIMSlashCommand(String command, String label) {
        this.command = command;
        this.label = label;
    }

    public String command() {
        return command;
    }

    public String label() {
        return label;
    }
}
