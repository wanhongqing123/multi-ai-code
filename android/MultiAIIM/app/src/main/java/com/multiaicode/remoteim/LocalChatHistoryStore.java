package com.multiaicode.remoteim;

import java.io.BufferedReader;
import java.io.BufferedWriter;
import java.io.File;
import java.io.FileReader;
import java.io.FileWriter;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.Base64;

public final class LocalChatHistoryStore {
    private final File baseDirectory;

    public LocalChatHistoryStore(File baseDirectory) {
        this.baseDirectory = baseDirectory;
    }

    public void save(ChatState state) throws IOException {
        if (!baseDirectory.exists() && !baseDirectory.mkdirs()) {
            throw new IOException("failed to create history directory");
        }

        try (BufferedWriter writer = new BufferedWriter(new FileWriter(historyFile(state.ownerUserId())))) {
            for (RemoteIMContact contact : state.contacts()) {
                writer.write("CONTACT");
                writer.write('\t');
                writer.write(encode(contact.userId()));
                writer.write('\t');
                writer.write(encode(contact.displayName()));
                writer.newLine();
            }

            for (RemoteIMMessage message : state.messages()) {
                writeMessage(writer, message);
            }
        }
    }

    public ChatState load(String ownerUserId) throws IOException {
        ChatState state = new ChatState(ownerUserId);
        File file = historyFile(ownerUserId);
        if (!file.exists()) return state;

        try (BufferedReader reader = new BufferedReader(new FileReader(file))) {
            String line;
            while ((line = reader.readLine()) != null) {
                String[] parts = line.split("\t", -1);
                if (parts.length == 0) continue;
                if ("CONTACT".equals(parts[0]) && parts.length >= 3) {
                    state.upsertContact(new RemoteIMContact(decode(parts[1]), decode(parts[2])));
                } else if ("MESSAGE".equals(parts[0]) && parts.length >= 14) {
                    state.addRestoredMessage(readMessage(parts));
                }
            }
        }

        return state;
    }

    private File historyFile(String ownerUserId) {
        return new File(baseDirectory, safeFileName(ownerUserId) + ".tsv");
    }

    private void writeMessage(BufferedWriter writer, RemoteIMMessage message) throws IOException {
        RemoteIMImageAttachment image = message.imageAttachment();
        RemoteIMVoiceAttachment voice = message.voiceAttachment();
        writer.write("MESSAGE");
        writer.write('\t');
        writer.write(encode(message.id()));
        writer.write('\t');
        writer.write(encode(message.fromUserId()));
        writer.write('\t');
        writer.write(encode(message.toUserId()));
        writer.write('\t');
        writer.write(encode(message.text()));
        writer.write('\t');
        writer.write(message.direction().name());
        writer.write('\t');
        writer.write(message.status().name());
        writer.write('\t');
        writer.write(Long.toString(message.createdAtMillis()));
        writer.write('\t');
        writer.write(encode(image == null ? "" : image.localPath()));
        writer.write('\t');
        writer.write(Integer.toString(image == null ? 0 : image.width()));
        writer.write('\t');
        writer.write(Integer.toString(image == null ? 0 : image.height()));
        writer.write('\t');
        writer.write(Long.toString(image == null ? 0 : image.sizeBytes()));
        writer.write('\t');
        writer.write(encode(voice == null ? "" : voice.localPath()));
        writer.write('\t');
        writer.write(Integer.toString(voice == null ? 0 : voice.durationSeconds()));
        writer.newLine();
    }

    private RemoteIMMessage readMessage(String[] parts) {
        RemoteIMImageAttachment image = null;
        String imagePath = decode(parts[8]);
        if (!imagePath.isEmpty()) {
            image = new RemoteIMImageAttachment(
                imagePath,
                parseInt(parts[9]),
                parseInt(parts[10]),
                parseLong(parts[11])
            );
        }

        RemoteIMVoiceAttachment voice = null;
        String voicePath = decode(parts[12]);
        if (!voicePath.isEmpty()) {
            voice = new RemoteIMVoiceAttachment(voicePath, parseInt(parts[13]));
        }

        return new RemoteIMMessage(
            decode(parts[1]),
            decode(parts[2]),
            decode(parts[3]),
            decode(parts[4]),
            RemoteIMMessage.Direction.valueOf(parts[5]),
            RemoteIMMessage.Status.valueOf(parts[6]),
            parseLong(parts[7]),
            image,
            voice
        );
    }

    private static String encode(String value) {
        return Base64.getUrlEncoder().encodeToString(
            (value == null ? "" : value).getBytes(StandardCharsets.UTF_8)
        );
    }

    private static String decode(String value) {
        if (value == null || value.isEmpty()) return "";
        return new String(Base64.getUrlDecoder().decode(value), StandardCharsets.UTF_8);
    }

    private static int parseInt(String value) {
        try {
            return Integer.parseInt(value);
        } catch (NumberFormatException err) {
            return 0;
        }
    }

    private static long parseLong(String value) {
        try {
            return Long.parseLong(value);
        } catch (NumberFormatException err) {
            return 0;
        }
    }

    private static String safeFileName(String value) {
        return (value == null ? "" : value.trim()).replaceAll("[^A-Za-z0-9._-]", "_");
    }
}
