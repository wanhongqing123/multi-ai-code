package com.multiaicode.remoteim;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.util.Properties;

public final class LocalSettingsStore {
    private static final String LOGIN_USER_ID = "loginUserId";

    private final File file;

    public LocalSettingsStore(File file) {
        this.file = file;
    }

    public RemoteIMSettings load() throws IOException {
        if (!file.exists()) return RemoteIMSettings.empty();
        Properties properties = new Properties();
        try (FileInputStream input = new FileInputStream(file)) {
            properties.load(input);
        }
        return new RemoteIMSettings(properties.getProperty(LOGIN_USER_ID, ""));
    }

    public void save(RemoteIMSettings settings) throws IOException {
        File parent = file.getParentFile();
        if (parent != null && !parent.exists() && !parent.mkdirs()) {
            throw new IOException("failed to create settings directory");
        }
        Properties properties = new Properties();
        properties.setProperty(LOGIN_USER_ID, settings.loginUserId());
        try (FileOutputStream output = new FileOutputStream(file)) {
            properties.store(output, "Multi-AI Code Remote IM settings");
        }
    }
}
