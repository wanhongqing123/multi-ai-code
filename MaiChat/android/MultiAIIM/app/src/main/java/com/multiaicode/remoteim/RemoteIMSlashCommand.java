package com.multiaicode.remoteim;

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
