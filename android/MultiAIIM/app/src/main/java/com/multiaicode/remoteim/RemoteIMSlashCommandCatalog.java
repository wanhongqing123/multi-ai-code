package com.multiaicode.remoteim;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

public final class RemoteIMSlashCommandCatalog {
    private static final List<RemoteIMSlashCommand> COMMANDS = List.of(
        new RemoteIMSlashCommand("/status", "查看状态"),
        new RemoteIMSlashCommand("/plan", "切换 Plan"),
        new RemoteIMSlashCommand("/build", "切换 Build"),
        new RemoteIMSlashCommand("/models", "模型列表"),
        new RemoteIMSlashCommand("/model ", "切换模型"),
        new RemoteIMSlashCommand("/goal ", "管理 Goal"),
        new RemoteIMSlashCommand("/help", "命令帮助")
    );

    private RemoteIMSlashCommandCatalog() {
    }

    public static List<RemoteIMSlashCommand> suggestions(String input) {
        String query = input.trim();
        if (!query.startsWith("/")) return Collections.emptyList();

        List<RemoteIMSlashCommand> matches = new ArrayList<>();
        for (RemoteIMSlashCommand command : COMMANDS) {
            if (command.command().startsWith(query)) {
                matches.add(command);
            }
        }
        return matches;
    }
}
