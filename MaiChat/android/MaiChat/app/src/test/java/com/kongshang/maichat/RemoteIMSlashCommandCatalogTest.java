package com.kongshang.maichat;

import static org.junit.Assert.assertEquals;

import org.junit.Test;

import java.util.List;

public class RemoteIMSlashCommandCatalogTest {
    @Test
    public void returnsAllCommandsForSlash() {
        List<RemoteIMSlashCommand> commands = RemoteIMSlashCommandCatalog.suggestions("/");

        assertEquals(12, commands.size());
        assertEquals("/status", commands.get(0).command());
        assertEquals("/help", commands.get(11).command());
    }

    @Test
    public void filtersCommandsByPrefix() {
        List<RemoteIMSlashCommand> modelCommands = RemoteIMSlashCommandCatalog.suggestions("/mo");
        List<RemoteIMSlashCommand> goalCommands = RemoteIMSlashCommandCatalog.suggestions("/go");
        List<RemoteIMSlashCommand> btwCommands = RemoteIMSlashCommandCatalog.suggestions("/bt");
        List<RemoteIMSlashCommand> diffCommands = RemoteIMSlashCommandCatalog.suggestions("/di");
        List<RemoteIMSlashCommand> interruptCommands = RemoteIMSlashCommandCatalog.suggestions("/in");
        List<RemoteIMSlashCommand> compactCommands = RemoteIMSlashCommandCatalog.suggestions("/co");
        List<RemoteIMSlashCommand> clearCommands = RemoteIMSlashCommandCatalog.suggestions("/cl");

        assertEquals(2, modelCommands.size());
        assertEquals("/models", modelCommands.get(0).command());
        assertEquals("/model ", modelCommands.get(1).command());
        assertEquals(1, goalCommands.size());
        assertEquals("/goal ", goalCommands.get(0).command());
        assertEquals(1, btwCommands.size());
        assertEquals("/btw ", btwCommands.get(0).command());
        assertEquals(1, diffCommands.size());
        assertEquals("/diff ", diffCommands.get(0).command());
        assertEquals(1, interruptCommands.size());
        assertEquals("/interrupt", interruptCommands.get(0).command());
        assertEquals(1, compactCommands.size());
        assertEquals("/compact", compactCommands.get(0).command());
        assertEquals(1, clearCommands.size());
        assertEquals("/clear", clearCommands.get(0).command());
    }

    @Test
    public void ignoresNonCommandInput() {
        assertEquals(List.of(), RemoteIMSlashCommandCatalog.suggestions("hello"));
    }
}
