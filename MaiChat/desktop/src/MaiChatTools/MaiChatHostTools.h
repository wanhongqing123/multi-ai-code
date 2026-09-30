#pragma once

class MaiToolRegistry;
class RemoteIMApplication;

// Register tools that are owned by the MaiChat host. MaiAgent remains unaware
// of Qt, contacts, conversations, and message delivery; another application
// embedding MaiAgent can register a completely different tool set through the
// same registry extension point.
void registerMaiChatHostTools(MaiToolRegistry &registry,
                              RemoteIMApplication &app);
