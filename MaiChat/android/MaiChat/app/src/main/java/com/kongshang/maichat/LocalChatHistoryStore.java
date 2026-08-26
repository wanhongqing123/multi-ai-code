package com.kongshang.maichat;

import java.io.BufferedReader;
import java.io.BufferedWriter;
import java.io.File;
import java.io.FileReader;
import java.io.FileWriter;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Base64;
import java.util.List;

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
        RemoteIMFileAttachment file = message.fileAttachment();
        RemoteIMVideoAttachment video = message.videoAttachment();
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
        writer.write('\t');
        writer.write(encode(file == null ? "" : file.localPath()));
        writer.write('\t');
        writer.write(encode(file == null ? "" : file.fileName()));
        writer.write('\t');
        writer.write(encode(file == null ? "" : file.mimeType()));
        writer.write('\t');
        writer.write(Long.toString(file == null ? 0 : file.sizeBytes()));
        writer.write('	');
        writer.write(encode(video == null ? "" : video.localPath()));
        writer.write('	');
        writer.write(encode(video == null ? "" : video.coverPath()));
        writer.write('	');
        writer.write(Integer.toString(video == null ? 0 : video.durationSeconds()));
        writer.write('	');
        writer.write(Integer.toString(video == null ? 0 : video.width()));
        writer.write('	');
        writer.write(Integer.toString(video == null ? 0 : video.height()));
        writer.write('	');
        writer.write(Long.toString(video == null ? 0 : video.sizeBytes()));
        writer.write('\t');
        writer.write(message.origin().wireValue());
        writer.write('\t');
        writer.write(encode(message.approvalRequest() == null ? "" : message.approvalRequest().token()));
        writer.write('\t');
        writer.write(encode(message.approvalRequest() == null
            ? ""
            : approvalActions(message.approvalRequest().actions())));
        writer.write('\t');
        writer.write(encode(message.approvalDecision() == null ? "" : message.approvalDecision().token()));
        writer.write('\t');
        writer.write(message.approvalDecision() == null
            ? ""
            : message.approvalDecision().action().wireValue());
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

        RemoteIMFileAttachment file = null;
        if (parts.length >= 18) {
            String filePath = decode(parts[14]);
            if (!filePath.isEmpty()) {
                file = new RemoteIMFileAttachment(
                    filePath,
                    decode(parts[15]),
                    decode(parts[16]),
                    parseLong(parts[17])
                );
            }
        }

        RemoteIMVideoAttachment video = null;
        if (parts.length >= 24) {
            String videoPath = decode(parts[18]);
            if (!videoPath.isEmpty()) {
                video = new RemoteIMVideoAttachment(
                    videoPath,
                    decode(parts[19]),
                    parseInt(parts[20]),
                    parseInt(parts[21]),
                    parseInt(parts[22]),
                    parseLong(parts[23])
                );
            }
        }

        RemoteIMOrigin origin = parts.length >= 25
            ? RemoteIMOrigin.fromWireValue(parts[24])
            : RemoteIMOrigin.HUMAN;
        RemoteIMApprovalRequest approvalRequest = parts.length >= 27
            ? approvalRequest(decode(parts[25]), decode(parts[26]))
            : null;
        RemoteIMApprovalDecision approvalDecision = parts.length >= 29
            ? approvalDecision(decode(parts[27]), parts[28])
            : null;

        return new RemoteIMMessage(
            decode(parts[1]),
            null,
            decode(parts[2]),
            decode(parts[3]),
            decode(parts[4]),
            RemoteIMMessage.Direction.valueOf(parts[5]),
            RemoteIMMessage.Status.valueOf(parts[6]),
            parseLong(parts[7]),
            image,
            voice,
            file,
            video,
            origin,
            approvalRequest,
            approvalDecision
        );
    }

    private static String approvalActions(List<RemoteIMApprovalAction> actions) {
        StringBuilder value = new StringBuilder();
        for (RemoteIMApprovalAction action : actions) {
            if (value.length() > 0) value.append(',');
            value.append(action.wireValue());
        }
        return value.toString();
    }

    private static RemoteIMApprovalRequest approvalRequest(String token, String rawActions) {
        if (token.isEmpty() || rawActions.isEmpty()) return null;
        List<RemoteIMApprovalAction> actions = new ArrayList<>();
        for (String rawAction : rawActions.split(",", -1)) {
            RemoteIMApprovalAction action = RemoteIMApprovalAction.fromWireValue(rawAction);
            if (action == null) return null;
            actions.add(action);
        }
        try {
            return new RemoteIMApprovalRequest(token, actions);
        } catch (IllegalArgumentException error) {
            return null;
        }
    }

    private static RemoteIMApprovalDecision approvalDecision(String token, String rawAction) {
        RemoteIMApprovalAction action = RemoteIMApprovalAction.fromWireValue(rawAction);
        if (token.isEmpty() || action == null) return null;
        try {
            return new RemoteIMApprovalDecision(token, action);
        } catch (IllegalArgumentException error) {
            return null;
        }
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
