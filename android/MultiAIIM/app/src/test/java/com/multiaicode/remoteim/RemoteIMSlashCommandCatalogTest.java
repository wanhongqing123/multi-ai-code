package com.multiaicode.remoteim;

import static org.junit.Assert.assertEquals;

import org.junit.Test;

import java.util.List;

public class RemoteIMSlashCommandCatalogTest {
    @Test
    public void returnsAllCommandsForSlash() {
        List<RemoteIMSlashCommand> commands = RemoteIMSlashCommandCatalog.suggestions("/");

        assertEquals(7, commands.size());
        assertEquals("/status", commands.get(0).command());
        assertEquals("/help", commands.get(6).command());
    }

    @Test
    public void filtersCommandsByPrefix() {
        List<RemoteIMSlashCommand> modelCommands = RemoteIMSlashCommandCatalog.suggestions("/mo");
        List<RemoteIMSlashCommand> goalCommands = RemoteIMSlashCommandCatalog.suggestions("/go");

        assertEquals(2, modelCommands.size());
        assertEquals("/models", modelCommands.get(0).command());
        assertEquals("/model ", modelCommands.get(1).command());
        assertEquals(1, goalCommands.size());
        assertEquals("/goal ", goalCommands.get(0).command());
    }

    @Test
    public void ignoresNonCommandInput() {
        assertEquals(List.of(), RemoteIMSlashCommandCatalog.suggestions("hello"));
    }
}
