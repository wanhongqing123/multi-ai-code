# Android Native IM Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 打通 `android/MultiAIIM` 的真实 Native IM 登录、文本、图片、语音发送和接收链路。

**Architecture:** Android UI 继续使用现有 `MainActivity` 和本地 `ChatState`，新增 `RemoteIMClient` 抽象隔离 SDK。`RemoteIMSessionController` 负责把 UI 操作、本地历史、媒体缓存和 SDK 回调串起来；真实 SDK 只出现在 `NativeRemoteIMClient`。

**Tech Stack:** Android Java, Gradle, JUnit 4, Tencent IM Android SDK `com.tencent.imsdk:imsdk-plus:9.0.7654`.

---

## File Structure

- Modify: `android/MultiAIIM/app/build.gradle`
  - Add Android IM SDK dependency.
- Modify: `android/MultiAIIM/app/src/main/AndroidManifest.xml`
  - Add network permission if absent.
- Create: `android/MultiAIIM/app/src/main/java/com/multiaicode/remoteim/RemoteIMCredentialDefaults.java`
  - Mirrors the iOS built-in SDK App ID and secret behavior.
- Create: `android/MultiAIIM/app/src/main/java/com/multiaicode/remoteim/TencentUserSigGenerator.java`
  - Generates UserSig locally for parity with iOS.
- Create: `android/MultiAIIM/app/src/main/java/com/multiaicode/remoteim/RemoteIMClient.java`
  - SDK-independent client interface.
- Create: `android/MultiAIIM/app/src/main/java/com/multiaicode/remoteim/RemoteIMClientListener.java`
  - Incoming message and connection-state callback interface.
- Create: `android/MultiAIIM/app/src/main/java/com/multiaicode/remoteim/RemoteIMResultCallback.java`
  - Small async result callback used by client and controller.
- Create: `android/MultiAIIM/app/src/main/java/com/multiaicode/remoteim/IncomingRemoteIMText.java`
- Create: `android/MultiAIIM/app/src/main/java/com/multiaicode/remoteim/IncomingRemoteIMImage.java`
- Create: `android/MultiAIIM/app/src/main/java/com/multiaicode/remoteim/IncomingRemoteIMVoice.java`
- Create: `android/MultiAIIM/app/src/main/java/com/multiaicode/remoteim/FakeRemoteIMClient.java`
  - Test double.
- Create: `android/MultiAIIM/app/src/main/java/com/multiaicode/remoteim/NativeRemoteIMClient.java`
  - Real Android SDK adapter.
- Modify: `android/MultiAIIM/app/src/main/java/com/multiaicode/remoteim/RemoteIMSessionController.java`
  - Inject client, connect on login, send through SDK, receive SDK events.
- Modify: `android/MultiAIIM/app/src/main/java/com/multiaicode/remoteim/RemoteIMMediaStore.java`
  - Add cache file helpers for SDK-downloaded images and voice.
- Modify: `android/MultiAIIM/app/src/main/java/com/multiaicode/remoteim/MainActivity.java`
  - Use native client, render connection state, refresh on SDK callbacks.
- Create: `android/MultiAIIM/app/src/test/java/com/multiaicode/remoteim/TencentUserSigGeneratorTest.java`
- Create: `android/MultiAIIM/app/src/test/java/com/multiaicode/remoteim/FakeRemoteIMClientTest.java`
- Modify: `android/MultiAIIM/app/src/test/java/com/multiaicode/remoteim/RemoteIMSessionControllerTest.java`
  - Update tests for async client success, failure, and incoming events.

## Task 1: Add Credential and UserSig Support

**Files:**
- Create: `android/MultiAIIM/app/src/main/java/com/multiaicode/remoteim/RemoteIMCredentialDefaults.java`
- Create: `android/MultiAIIM/app/src/main/java/com/multiaicode/remoteim/TencentUserSigGenerator.java`
- Create: `android/MultiAIIM/app/src/test/java/com/multiaicode/remoteim/TencentUserSigGeneratorTest.java`

- [ ] **Step 1: Write the UserSig tests**

Create `android/MultiAIIM/app/src/test/java/com/multiaicode/remoteim/TencentUserSigGeneratorTest.java`:

```java
package com.multiaicode.remoteim;

import static org.junit.Assert.assertTrue;

import org.junit.Test;

public class TencentUserSigGeneratorTest {
    @Test
    public void generatesNonEmptySigForBuiltInCredential() throws Exception {
        String sig = TencentUserSigGenerator.generate(
            RemoteIMCredentialDefaults.SDK_APP_ID,
            "android-user",
            RemoteIMCredentialDefaults.USER_SIG_SECRET_KEY
        );

        assertTrue(sig.length() > 80);
        assertTrue(sig.contains("."));
    }

    @Test(expected = IllegalArgumentException.class)
    public void rejectsBlankUserId() throws Exception {
        TencentUserSigGenerator.generate(
            RemoteIMCredentialDefaults.SDK_APP_ID,
            " ",
            RemoteIMCredentialDefaults.USER_SIG_SECRET_KEY
        );
    }
}
```

- [ ] **Step 2: Run the test and verify it fails**

Run:

```bash
cd android/MultiAIIM
./gradlew testDebugUnitTest --tests 'com.multiaicode.remoteim.TencentUserSigGeneratorTest'
```

Expected: fail because `TencentUserSigGenerator` and `RemoteIMCredentialDefaults` do not exist.

- [ ] **Step 3: Add credential defaults**

Create `android/MultiAIIM/app/src/main/java/com/multiaicode/remoteim/RemoteIMCredentialDefaults.java`:

```java
package com.multiaicode.remoteim;

public final class RemoteIMCredentialDefaults {
    public static final int SDK_APP_ID = 1600148979;
    public static final String USER_SIG_SECRET_KEY =
        "aa18d554f5e4a235640745e98145e187977f87770b812b2b4f10ef032bd73861";

    private RemoteIMCredentialDefaults() {
    }
}
```

- [ ] **Step 4: Add UserSig generator**

Create `android/MultiAIIM/app/src/main/java/com/multiaicode/remoteim/TencentUserSigGenerator.java`:

```java
package com.multiaicode.remoteim;

import java.nio.charset.StandardCharsets;
import java.util.Base64;
import java.util.Locale;
import java.util.zip.Deflater;

import javax.crypto.Mac;
import javax.crypto.spec.SecretKeySpec;

public final class TencentUserSigGenerator {
    private static final int EXPIRE_SECONDS = 604800;

    private TencentUserSigGenerator() {
    }

    public static String generate(int sdkAppId, String userId, String secretKey) throws Exception {
        String cleanUserId = userId == null ? "" : userId.trim();
        String cleanSecretKey = secretKey == null ? "" : secretKey.trim();
        if (sdkAppId <= 0) throw new IllegalArgumentException("sdkAppId is required");
        if (cleanUserId.isEmpty()) throw new IllegalArgumentException("userId is required");
        if (cleanSecretKey.isEmpty()) throw new IllegalArgumentException("secretKey is required");

        long current = System.currentTimeMillis() / 1000;
        String payload = String.format(
            Locale.US,
            "{"
                + "\"TLS.ver\":\"2.0\","
                + "\"TLS.identifier\":\"%s\","
                + "\"TLS.sdkappid\":%d,"
                + "\"TLS.expire\":%d,"
                + "\"TLS.time\":%d"
                + "}",
            escapeJson(cleanUserId),
            sdkAppId,
            EXPIRE_SECONDS,
            current
        );
        String sig = hmacSha256(
            "TLS.identifier:" + cleanUserId + "\n"
                + "TLS.sdkappid:" + sdkAppId + "\n"
                + "TLS.time:" + current + "\n"
                + "TLS.expire:" + EXPIRE_SECONDS + "\n",
            cleanSecretKey
        );
        String signedPayload = payload.substring(0, payload.length() - 1)
            + ",\"TLS.sig\":\"" + escapeJson(sig) + "\"}";
        return base64UrlEncode(deflate(signedPayload.getBytes(StandardCharsets.UTF_8)));
    }

    private static String hmacSha256(String content, String secretKey) throws Exception {
        Mac mac = Mac.getInstance("HmacSHA256");
        mac.init(new SecretKeySpec(secretKey.getBytes(StandardCharsets.UTF_8), "HmacSHA256"));
        return Base64.getEncoder().encodeToString(mac.doFinal(content.getBytes(StandardCharsets.UTF_8)));
    }

    private static byte[] deflate(byte[] input) {
        Deflater deflater = new Deflater();
        deflater.setInput(input);
        deflater.finish();
        byte[] buffer = new byte[4096];
        int length = deflater.deflate(buffer);
        deflater.end();
        byte[] output = new byte[length];
        System.arraycopy(buffer, 0, output, 0, length);
        return output;
    }

    private static String base64UrlEncode(byte[] input) {
        return Base64.getEncoder().encodeToString(input)
            .replace('+', '*')
            .replace('/', '-')
            .replace('=', '_');
    }

    private static String escapeJson(String value) {
        return value.replace("\\", "\\\\").replace("\"", "\\\"");
    }
}
```

- [ ] **Step 5: Run the tests and commit**

Run:

```bash
cd android/MultiAIIM
./gradlew testDebugUnitTest --tests 'com.multiaicode.remoteim.TencentUserSigGeneratorTest'
```

Expected: pass.

Commit:

```bash
git add android/MultiAIIM/app/src/main/java/com/multiaicode/remoteim/RemoteIMCredentialDefaults.java \
  android/MultiAIIM/app/src/main/java/com/multiaicode/remoteim/TencentUserSigGenerator.java \
  android/MultiAIIM/app/src/test/java/com/multiaicode/remoteim/TencentUserSigGeneratorTest.java
git commit -F /tmp/android-im-task1.msg
```

Use this commit message in `/tmp/android-im-task1.msg`:

```text
OPTIMIZE: 添加 Android IM 内置凭证生成
EFFECTION: Android IM 登录前可生成内置 UserSig
TESTPOINT: Android UserSig 生成单测通过
```

## Task 2: Add Client Interfaces and Fake Client

**Files:**
- Create: `android/MultiAIIM/app/src/main/java/com/multiaicode/remoteim/RemoteIMClient.java`
- Create: `android/MultiAIIM/app/src/main/java/com/multiaicode/remoteim/RemoteIMClientListener.java`
- Create: `android/MultiAIIM/app/src/main/java/com/multiaicode/remoteim/RemoteIMResultCallback.java`
- Create: `android/MultiAIIM/app/src/main/java/com/multiaicode/remoteim/IncomingRemoteIMText.java`
- Create: `android/MultiAIIM/app/src/main/java/com/multiaicode/remoteim/IncomingRemoteIMImage.java`
- Create: `android/MultiAIIM/app/src/main/java/com/multiaicode/remoteim/IncomingRemoteIMVoice.java`
- Create: `android/MultiAIIM/app/src/main/java/com/multiaicode/remoteim/FakeRemoteIMClient.java`
- Create: `android/MultiAIIM/app/src/test/java/com/multiaicode/remoteim/FakeRemoteIMClientTest.java`

- [ ] **Step 1: Write fake client tests**

Create `android/MultiAIIM/app/src/test/java/com/multiaicode/remoteim/FakeRemoteIMClientTest.java`:

```java
package com.multiaicode.remoteim;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertTrue;

import org.junit.Test;

public class FakeRemoteIMClientTest {
    @Test
    public void recordsConnectAndSendCalls() {
        FakeRemoteIMClient client = new FakeRemoteIMClient();
        ResultCapture<Void> connect = new ResultCapture<>();
        ResultCapture<Void> send = new ResultCapture<>();

        client.connect(1, "android-user", "sig", connect);
        client.sendText("mac-office", "ping", send);

        assertTrue(connect.success);
        assertTrue(send.success);
        assertEquals("android-user", client.connectedUserId());
        assertEquals("mac-office", client.lastTextPeerId());
        assertEquals("ping", client.lastText());
    }

    @Test
    public void dispatchesIncomingTextToListener() {
        FakeRemoteIMClient client = new FakeRemoteIMClient();
        final String[] received = new String[2];
        client.setListener(new RemoteIMClientListener() {
            @Override public void onIncomingText(IncomingRemoteIMText event) {
                received[0] = event.fromUserId();
                received[1] = event.text();
            }
            @Override public void onIncomingImage(IncomingRemoteIMImage event) {}
            @Override public void onIncomingVoice(IncomingRemoteIMVoice event) {}
            @Override public void onDisconnected() {}
        });

        client.emitIncomingText("mac-office", "done");

        assertEquals("mac-office", received[0]);
        assertEquals("done", received[1]);
    }

    private static final class ResultCapture<T> implements RemoteIMResultCallback<T> {
        boolean success;
        Exception error;
        @Override public void onSuccess(T value) { success = true; }
        @Override public void onError(Exception error) { this.error = error; }
    }
}
```

- [ ] **Step 2: Run tests and verify failure**

Run:

```bash
cd android/MultiAIIM
./gradlew testDebugUnitTest --tests 'com.multiaicode.remoteim.FakeRemoteIMClientTest'
```

Expected: fail because the client abstraction does not exist.

- [ ] **Step 3: Add event and callback classes**

Create `RemoteIMResultCallback.java`:

```java
package com.multiaicode.remoteim;

public interface RemoteIMResultCallback<T> {
    void onSuccess(T value);
    void onError(Exception error);
}
```

Create `IncomingRemoteIMText.java`:

```java
package com.multiaicode.remoteim;

public final class IncomingRemoteIMText {
    private final String fromUserId;
    private final String text;

    public IncomingRemoteIMText(String fromUserId, String text) {
        this.fromUserId = fromUserId == null ? "" : fromUserId.trim();
        this.text = text == null ? "" : text.trim();
    }

    public String fromUserId() { return fromUserId; }
    public String text() { return text; }
}
```

Create `IncomingRemoteIMImage.java`:

```java
package com.multiaicode.remoteim;

public final class IncomingRemoteIMImage {
    private final String fromUserId;
    private final String localPath;
    private final int width;
    private final int height;
    private final long sizeBytes;

    public IncomingRemoteIMImage(String fromUserId, String localPath, int width, int height, long sizeBytes) {
        this.fromUserId = fromUserId == null ? "" : fromUserId.trim();
        this.localPath = localPath == null ? "" : localPath.trim();
        this.width = Math.max(0, width);
        this.height = Math.max(0, height);
        this.sizeBytes = Math.max(0L, sizeBytes);
    }

    public String fromUserId() { return fromUserId; }
    public String localPath() { return localPath; }
    public int width() { return width; }
    public int height() { return height; }
    public long sizeBytes() { return sizeBytes; }
}
```

Create `IncomingRemoteIMVoice.java`:

```java
package com.multiaicode.remoteim;

public final class IncomingRemoteIMVoice {
    private final String fromUserId;
    private final String localPath;
    private final int durationSeconds;

    public IncomingRemoteIMVoice(String fromUserId, String localPath, int durationSeconds) {
        this.fromUserId = fromUserId == null ? "" : fromUserId.trim();
        this.localPath = localPath == null ? "" : localPath.trim();
        this.durationSeconds = Math.max(1, durationSeconds);
    }

    public String fromUserId() { return fromUserId; }
    public String localPath() { return localPath; }
    public int durationSeconds() { return durationSeconds; }
}
```

- [ ] **Step 4: Add client interfaces and fake implementation**

Create `RemoteIMClientListener.java`:

```java
package com.multiaicode.remoteim;

public interface RemoteIMClientListener {
    void onIncomingText(IncomingRemoteIMText event);
    void onIncomingImage(IncomingRemoteIMImage event);
    void onIncomingVoice(IncomingRemoteIMVoice event);
    void onDisconnected();
}
```

Create `RemoteIMClient.java`:

```java
package com.multiaicode.remoteim;

public interface RemoteIMClient {
    void setListener(RemoteIMClientListener listener);
    void connect(int sdkAppId, String userId, String userSig, RemoteIMResultCallback<Void> callback);
    void disconnect(RemoteIMResultCallback<Void> callback);
    void sendText(String peerId, String text, RemoteIMResultCallback<Void> callback);
    void sendImage(String peerId, String localPath, RemoteIMResultCallback<Void> callback);
    void sendVoice(String peerId, String localPath, int durationSeconds, RemoteIMResultCallback<Void> callback);
}
```

Create `FakeRemoteIMClient.java`:

```java
package com.multiaicode.remoteim;

public final class FakeRemoteIMClient implements RemoteIMClient {
    private RemoteIMClientListener listener;
    private String connectedUserId = "";
    private String lastTextPeerId = "";
    private String lastText = "";
    private Exception nextError;

    @Override
    public void setListener(RemoteIMClientListener listener) {
        this.listener = listener;
    }

    @Override
    public void connect(int sdkAppId, String userId, String userSig, RemoteIMResultCallback<Void> callback) {
        connectedUserId = userId == null ? "" : userId.trim();
        complete(callback);
    }

    @Override
    public void disconnect(RemoteIMResultCallback<Void> callback) {
        connectedUserId = "";
        complete(callback);
    }

    @Override
    public void sendText(String peerId, String text, RemoteIMResultCallback<Void> callback) {
        lastTextPeerId = peerId == null ? "" : peerId.trim();
        lastText = text == null ? "" : text.trim();
        complete(callback);
    }

    @Override
    public void sendImage(String peerId, String localPath, RemoteIMResultCallback<Void> callback) {
        complete(callback);
    }

    @Override
    public void sendVoice(String peerId, String localPath, int durationSeconds, RemoteIMResultCallback<Void> callback) {
        complete(callback);
    }

    public String connectedUserId() { return connectedUserId; }
    public String lastTextPeerId() { return lastTextPeerId; }
    public String lastText() { return lastText; }
    public void failNext(Exception error) { nextError = error; }

    public void emitIncomingText(String fromUserId, String text) {
        if (listener != null) listener.onIncomingText(new IncomingRemoteIMText(fromUserId, text));
    }

    public void emitIncomingImage(String fromUserId, String localPath, int width, int height, long sizeBytes) {
        if (listener != null) {
            listener.onIncomingImage(new IncomingRemoteIMImage(fromUserId, localPath, width, height, sizeBytes));
        }
    }

    public void emitIncomingVoice(String fromUserId, String localPath, int durationSeconds) {
        if (listener != null) {
            listener.onIncomingVoice(new IncomingRemoteIMVoice(fromUserId, localPath, durationSeconds));
        }
    }

    private void complete(RemoteIMResultCallback<Void> callback) {
        if (nextError != null) {
            Exception error = nextError;
            nextError = null;
            callback.onError(error);
        } else {
            callback.onSuccess(null);
        }
    }
}
```

- [ ] **Step 5: Run tests and commit**

Run:

```bash
cd android/MultiAIIM
./gradlew testDebugUnitTest --tests 'com.multiaicode.remoteim.FakeRemoteIMClientTest'
```

Expected: pass.

Commit:

```bash
git add android/MultiAIIM/app/src/main/java/com/multiaicode/remoteim/RemoteIMClient.java \
  android/MultiAIIM/app/src/main/java/com/multiaicode/remoteim/RemoteIMClientListener.java \
  android/MultiAIIM/app/src/main/java/com/multiaicode/remoteim/RemoteIMResultCallback.java \
  android/MultiAIIM/app/src/main/java/com/multiaicode/remoteim/IncomingRemoteIMText.java \
  android/MultiAIIM/app/src/main/java/com/multiaicode/remoteim/IncomingRemoteIMImage.java \
  android/MultiAIIM/app/src/main/java/com/multiaicode/remoteim/IncomingRemoteIMVoice.java \
  android/MultiAIIM/app/src/main/java/com/multiaicode/remoteim/FakeRemoteIMClient.java \
  android/MultiAIIM/app/src/test/java/com/multiaicode/remoteim/FakeRemoteIMClientTest.java
git commit -F /tmp/android-im-task2.msg
```

Use this commit message:

```text
OPTIMIZE: 抽象 Android IM 客户端接口
EFFECTION: Android IM 控制器可脱离真实 SDK 测试
TESTPOINT: Fake IM 客户端单测通过
```

## Task 3: Wire Controller to RemoteIMClient

**Files:**
- Modify: `android/MultiAIIM/app/src/main/java/com/multiaicode/remoteim/RemoteIMSessionController.java`
- Modify: `android/MultiAIIM/app/src/test/java/com/multiaicode/remoteim/RemoteIMSessionControllerTest.java`

- [ ] **Step 1: Update controller tests for real client behavior**

Modify `RemoteIMSessionControllerTest.java` to construct sessions with a fake client and add these tests:

```java
@Test
public void loginConnectsClientWithBuiltInCredential() throws Exception {
    Path root = Files.createTempDirectory("multi-ai-code-android-session-connect");
    FakeRemoteIMClient client = new FakeRemoteIMClient();
    RemoteIMSessionController session = newSession(root, client);

    session.login("android-user");

    assertFalse(session.requiresLogin());
    assertEquals("android-user", client.connectedUserId());
}

@Test
public void sendFailureMarksMessageFailedAndPersistsIt() throws Exception {
    Path root = Files.createTempDirectory("multi-ai-code-android-session-fail");
    FakeRemoteIMClient client = new FakeRemoteIMClient();
    RemoteIMSessionController session = newSession(root, client);
    session.login("android-user");
    client.failNext(new IOException("network failed"));

    RemoteIMMessage message = session.sendTextMessage("检查构建");

    assertEquals(RemoteIMMessage.Status.FAILED, message.status());
    RemoteIMSessionController restored = newSession(root, new FakeRemoteIMClient());
    assertEquals(RemoteIMMessage.Status.FAILED, restored.chatState().messagesWith("mac-office").get(0).status());
}

@Test
public void incomingSdkTextIsSavedToChatState() throws Exception {
    Path root = Files.createTempDirectory("multi-ai-code-android-session-incoming");
    FakeRemoteIMClient client = new FakeRemoteIMClient();
    RemoteIMSessionController session = newSession(root, client);
    session.login("android-user");

    client.emitIncomingText("mac-office", "处理完成");

    assertEquals("处理完成", session.chatState().messagesWith("mac-office").get(0).text());
    RemoteIMSessionController restored = newSession(root, new FakeRemoteIMClient());
    assertEquals("处理完成", restored.chatState().messagesWith("mac-office").get(0).text());
}

private RemoteIMSessionController newSession(Path root, RemoteIMClient client) throws Exception {
    return new RemoteIMSessionController(
        new LocalSettingsStore(root.resolve("settings.properties").toFile()),
        new LocalChatHistoryStore(root.resolve("history").toFile()),
        client
    );
}
```

- [ ] **Step 2: Run tests and verify failure**

Run:

```bash
cd android/MultiAIIM
./gradlew testDebugUnitTest --tests 'com.multiaicode.remoteim.RemoteIMSessionControllerTest'
```

Expected: fail because `RemoteIMSessionController` does not accept a client and still marks sends as sent locally.

- [ ] **Step 3: Refactor controller constructor and client listener**

Modify `RemoteIMSessionController.java` fields and constructor:

```java
private final RemoteIMClient client;

public RemoteIMSessionController(
    LocalSettingsStore settingsStore,
    LocalChatHistoryStore historyStore,
    RemoteIMClient client
) {
    this.settingsStore = settingsStore;
    this.historyStore = historyStore;
    this.client = client;
    this.client.setListener(new RemoteIMClientListener() {
        @Override public void onIncomingText(IncomingRemoteIMText event) { receive(event); }
        @Override public void onIncomingImage(IncomingRemoteIMImage event) { receive(event); }
        @Override public void onIncomingVoice(IncomingRemoteIMVoice event) { receive(event); }
        @Override public void onDisconnected() {}
    });
    settings = loadSettings();
    chatState = loadChatState();
    ensureDefaultContactIfNeeded();
}
```

Add private receive methods:

```java
private void receive(IncomingRemoteIMText event) {
    chatState.receiveText(event.text(), event.fromUserId());
    saveQuietly();
}

private void receive(IncomingRemoteIMImage event) {
    chatState.receiveImage(
        event.localPath(),
        event.fromUserId(),
        event.width(),
        event.height(),
        event.sizeBytes()
    );
    saveQuietly();
}

private void receive(IncomingRemoteIMVoice event) {
    chatState.receiveVoice(event.localPath(), event.durationSeconds(), event.fromUserId());
    saveQuietly();
}

private void saveQuietly() {
    try {
        saveChatState();
    } catch (IOException ignored) {
    }
}
```

- [ ] **Step 4: Connect on login and send through client**

Modify `login`:

```java
public void login(String loginUserId) throws IOException {
    saveChatState();
    settings = new RemoteIMSettings(loginUserId);
    settingsStore.save(settings);
    chatState = loadChatState();
    ensureDefaultContactIfNeeded();
    connectCurrentUser();
}

private void connectCurrentUser() {
    try {
        String userSig = TencentUserSigGenerator.generate(
            RemoteIMCredentialDefaults.SDK_APP_ID,
            settings.loginUserId(),
            RemoteIMCredentialDefaults.USER_SIG_SECRET_KEY
        );
        client.connect(
            RemoteIMCredentialDefaults.SDK_APP_ID,
            settings.loginUserId(),
            userSig,
            new RemoteIMResultCallback<Void>() {
                @Override public void onSuccess(Void value) {}
                @Override public void onError(Exception error) {}
            }
        );
    } catch (Exception ignored) {
    }
}
```

Modify send methods to queue first, save pending state, then call the client:

```java
public RemoteIMMessage sendTextMessage(String text) throws IOException {
    RemoteIMMessage message = chatState.queueOutgoingText(text);
    saveChatState();
    client.sendText(message.toUserId(), message.text(), statusCallback(message.id()));
    return message;
}

public RemoteIMMessage sendImageMessage(String localPath, int width, int height, long sizeBytes) throws IOException {
    RemoteIMMessage message = chatState.queueOutgoingImage(localPath, width, height, sizeBytes);
    saveChatState();
    client.sendImage(message.toUserId(), localPath, statusCallback(message.id()));
    return message;
}

public RemoteIMMessage sendVoiceMessage(String localPath, int durationSeconds) throws IOException {
    RemoteIMMessage message = chatState.queueOutgoingVoice(localPath, durationSeconds);
    saveChatState();
    client.sendVoice(message.toUserId(), localPath, durationSeconds, statusCallback(message.id()));
    return message;
}

private RemoteIMResultCallback<Void> statusCallback(String messageId) {
    return new RemoteIMResultCallback<Void>() {
        @Override public void onSuccess(Void value) {
            chatState.updateMessageStatus(messageId, RemoteIMMessage.Status.SENT);
            saveQuietly();
        }

        @Override public void onError(Exception error) {
            chatState.updateMessageStatus(messageId, RemoteIMMessage.Status.FAILED);
            saveQuietly();
        }
    };
}
```

- [ ] **Step 5: Run controller tests and commit**

Run:

```bash
cd android/MultiAIIM
./gradlew testDebugUnitTest --tests 'com.multiaicode.remoteim.RemoteIMSessionControllerTest'
```

Expected: pass.

Commit:

```bash
git add android/MultiAIIM/app/src/main/java/com/multiaicode/remoteim/RemoteIMSessionController.java \
  android/MultiAIIM/app/src/test/java/com/multiaicode/remoteim/RemoteIMSessionControllerTest.java
git commit -F /tmp/android-im-task3.msg
```

Use this commit message:

```text
OPTIMIZE: 接入 Android IM 控制器客户端
EFFECTION: Android 消息状态由 IM 客户端发送结果驱动
TESTPOINT: Android 会话控制器发送失败与接收消息单测通过
```

## Task 4: Add Native Android SDK Adapter

**Files:**
- Modify: `android/MultiAIIM/app/build.gradle`
- Modify: `android/MultiAIIM/app/src/main/AndroidManifest.xml`
- Modify: `android/MultiAIIM/app/src/main/java/com/multiaicode/remoteim/RemoteIMMediaStore.java`
- Create: `android/MultiAIIM/app/src/main/java/com/multiaicode/remoteim/NativeRemoteIMClient.java`

- [ ] **Step 1: Add SDK dependency and network permission**

Modify `android/MultiAIIM/app/build.gradle`:

```gradle
dependencies {
    implementation "com.tencent.imsdk:imsdk-plus:9.0.7654"
    testImplementation "junit:junit:4.13.2"
}
```

Ensure `AndroidManifest.xml` contains:

```xml
<uses-permission android:name="android.permission.INTERNET" />
<uses-permission android:name="android.permission.ACCESS_NETWORK_STATE" />
```

- [ ] **Step 2: Add SDK media cache helpers**

Modify `RemoteIMMediaStore.java` with:

```java
public File createIncomingImageFile(String remoteId, String extension) throws IOException {
    File directory = ensureDirectory("RemoteIMIncomingImage");
    return new File(directory, "remote-im-incoming-image-" + safeName(remoteId) + safeExtension(extension, ".jpg"));
}

public File createIncomingVoiceFile(String remoteId) throws IOException {
    File directory = ensureDirectory("RemoteIMIncomingVoice");
    return new File(directory, "remote-im-incoming-voice-" + safeName(remoteId) + ".m4a");
}

private static String safeName(String value) {
    String clean = value == null ? "" : value.replaceAll("[^A-Za-z0-9_-]", "_");
    return clean.isEmpty() ? String.valueOf(System.currentTimeMillis()) : clean;
}

private static String safeExtension(String value, String fallback) {
    String clean = value == null ? "" : value.trim().toLowerCase(Locale.US);
    if (!clean.startsWith(".")) clean = "." + clean;
    return clean.matches("\\.[a-z0-9]{1,8}") ? clean : fallback;
}
```

- [ ] **Step 3: Add NativeRemoteIMClient**

Create `NativeRemoteIMClient.java`:

```java
package com.multiaicode.remoteim;

import android.content.Context;

import com.tencent.imsdk.v2.V2TIMAdvancedMsgListener;
import com.tencent.imsdk.v2.V2TIMCallback;
import com.tencent.imsdk.v2.V2TIMImage;
import com.tencent.imsdk.v2.V2TIMImageElem;
import com.tencent.imsdk.v2.V2TIMManager;
import com.tencent.imsdk.v2.V2TIMMessage;
import com.tencent.imsdk.v2.V2TIMSendCallback;
import com.tencent.imsdk.v2.V2TIMSimpleMsgListener;
import com.tencent.imsdk.v2.V2TIMSoundElem;
import com.tencent.imsdk.v2.V2TIMUserInfo;

import java.io.File;

public final class NativeRemoteIMClient extends V2TIMAdvancedMsgListener implements RemoteIMClient {
    private final Context context;
    private final RemoteIMMediaStore mediaStore;
    private RemoteIMClientListener listener;
    private int initializedSdkAppId;

    public NativeRemoteIMClient(Context context, RemoteIMMediaStore mediaStore) {
        this.context = context.getApplicationContext();
        this.mediaStore = mediaStore;
    }

    @Override
    public void setListener(RemoteIMClientListener listener) {
        this.listener = listener;
    }

    @Override
    public void connect(int sdkAppId, String userId, String userSig, RemoteIMResultCallback<Void> callback) {
        if (initializedSdkAppId != sdkAppId) {
            boolean ok = V2TIMManager.getInstance().initSDK(context, sdkAppId, null);
            if (!ok) {
                callback.onError(new IllegalStateException("IM SDK init failed"));
                return;
            }
            initializedSdkAppId = sdkAppId;
        }
        V2TIMManager.getInstance().addAdvancedMsgListener(this);
        V2TIMManager.getInstance().addSimpleMsgListener(simpleMsgListener);
        V2TIMManager.getInstance().login(userId, userSig, callback(callback));
    }

    @Override
    public void disconnect(RemoteIMResultCallback<Void> callback) {
        V2TIMManager.getInstance().removeAdvancedMsgListener(this);
        V2TIMManager.getInstance().removeSimpleMsgListener(simpleMsgListener);
        V2TIMManager.getInstance().logout(callback(callback));
    }

    @Override
    public void sendText(String peerId, String text, RemoteIMResultCallback<Void> callback) {
        V2TIMManager.getInstance().sendC2CTextMessage(text, peerId, callback(callback));
    }

    @Override
    public void sendImage(String peerId, String localPath, RemoteIMResultCallback<Void> callback) {
        V2TIMMessage message = V2TIMManager.getInstance().createImageMessage(localPath);
        V2TIMManager.getInstance().sendMessage(message, peerId, null, 0, false, null, sendCallback(callback));
    }

    @Override
    public void sendVoice(String peerId, String localPath, int durationSeconds, RemoteIMResultCallback<Void> callback) {
        V2TIMMessage message = V2TIMManager.getInstance().createSoundMessage(localPath, durationSeconds);
        V2TIMManager.getInstance().sendMessage(message, peerId, null, 0, false, null, sendCallback(callback));
    }

    @Override
    public void onRecvNewMessage(V2TIMMessage msg) {
        if (msg == null || msg.isSelf()) return;
        String fromUserId = msg.getSender();
        if (fromUserId == null || fromUserId.trim().isEmpty()) fromUserId = msg.getUserID();
        if (fromUserId == null || fromUserId.trim().isEmpty()) return;
        if (msg.getImageElem() != null) handleImage(msg, fromUserId);
        if (msg.getSoundElem() != null) handleSound(msg, fromUserId);
    }

    private final V2TIMSimpleMsgListener simpleMsgListener = new V2TIMSimpleMsgListener() {
        @Override
        public void onRecvC2CTextMessage(String msgID, V2TIMUserInfo sender, String text) {
            if (listener == null || sender == null || sender.getUserID() == null || text == null) return;
            listener.onIncomingText(new IncomingRemoteIMText(sender.getUserID(), text));
        }
    };

    private void handleImage(V2TIMMessage msg, String fromUserId) {
        V2TIMImageElem elem = msg.getImageElem();
        if (elem == null || elem.getImageList() == null || elem.getImageList().isEmpty()) return;
        V2TIMImage image = elem.getImageList().get(0);
        try {
            String url = image.getUrl();
            String ext = url == null ? ".jpg" : url.substring(Math.max(url.lastIndexOf('.'), 0));
            File target = mediaStore.createIncomingImageFile(image.getUUID(), ext);
            image.downloadImage(target.getAbsolutePath(), null, new V2TIMCallback() {
                @Override public void onSuccess() {
                    if (listener != null) {
                        listener.onIncomingImage(new IncomingRemoteIMImage(
                            fromUserId,
                            target.getAbsolutePath(),
                            image.getWidth(),
                            image.getHeight(),
                            image.getSize()
                        ));
                    }
                }
                @Override public void onError(int code, String desc) {}
            });
        } catch (Exception ignored) {
        }
    }

    private void handleSound(V2TIMMessage msg, String fromUserId) {
        V2TIMSoundElem elem = msg.getSoundElem();
        if (elem == null) return;
        try {
            File target = mediaStore.createIncomingVoiceFile(elem.getUUID());
            elem.downloadSound(target.getAbsolutePath(), new V2TIMCallback() {
                @Override public void onSuccess() {
                    if (listener != null) {
                        listener.onIncomingVoice(new IncomingRemoteIMVoice(
                            fromUserId,
                            target.getAbsolutePath(),
                            elem.getDuration()
                        ));
                    }
                }
                @Override public void onError(int code, String desc) {}
            });
        } catch (Exception ignored) {
        }
    }

    private static V2TIMCallback callback(RemoteIMResultCallback<Void> callback) {
        return new V2TIMCallback() {
            @Override public void onSuccess() { callback.onSuccess(null); }
            @Override public void onError(int code, String desc) {
                callback.onError(new IllegalStateException(code + ": " + desc));
            }
        };
    }

    private static V2TIMSendCallback<V2TIMMessage> sendCallback(RemoteIMResultCallback<Void> callback) {
        return new V2TIMSendCallback<V2TIMMessage>() {
            @Override public void onProgress(int progress) {}
            @Override public void onSuccess(V2TIMMessage message) { callback.onSuccess(null); }
            @Override public void onError(int code, String desc) {
                callback.onError(new IllegalStateException(code + ": " + desc));
            }
        };
    }
}
```

- [ ] **Step 4: Compile Android**

Run:

```bash
cd android/MultiAIIM
./gradlew assembleDebug
```

Expected: build succeeds. If an SDK method signature differs in version `9.0.7654`, update only `NativeRemoteIMClient.java` and keep the `RemoteIMClient` interface unchanged.

- [ ] **Step 5: Commit**

```bash
git add android/MultiAIIM/app/build.gradle \
  android/MultiAIIM/app/src/main/AndroidManifest.xml \
  android/MultiAIIM/app/src/main/java/com/multiaicode/remoteim/RemoteIMMediaStore.java \
  android/MultiAIIM/app/src/main/java/com/multiaicode/remoteim/NativeRemoteIMClient.java
git commit -F /tmp/android-im-task4.msg
```

Use this commit message:

```text
OPTIMIZE: 接入 Android IM Native SDK
EFFECTION: Android 可通过 Native SDK 登录并发送 IM 消息
TESTPOINT: Android debug 包编译通过
```

## Task 5: Wire MainActivity to Native Client

**Files:**
- Modify: `android/MultiAIIM/app/src/main/java/com/multiaicode/remoteim/MainActivity.java`

- [ ] **Step 1: Replace local-only controller construction**

In `onCreate`, construct `RemoteIMMediaStore` before `RemoteIMSessionController`, then inject `NativeRemoteIMClient`:

```java
mediaStore = new RemoteIMMediaStore(getCacheDir());
session = new RemoteIMSessionController(
    new LocalSettingsStore(new File(getFilesDir(), "remote-im-settings/settings.properties")),
    new LocalChatHistoryStore(new File(getFilesDir(), "chat-history")),
    new NativeRemoteIMClient(getApplicationContext(), mediaStore)
);
```

- [ ] **Step 2: Refresh UI after sends**

Keep `sendText`, `sendPickedImage`, and `stopRecording` rendering after the local queue operation. The SDK callback will update status in the controller and persist history.

For `sendText`, the body stays:

```java
try {
    session.sendTextMessage(text);
    messageInput.setText("");
    render();
} catch (IOException err) {
    Toast.makeText(this, "文本消息发送失败", Toast.LENGTH_SHORT).show();
}
```

- [ ] **Step 3: Add a simple manual refresh after incoming callbacks**

Add this method:

```java
private void refreshFromImCallback() {
    runOnUiThread(this::render);
}
```

Extend `RemoteIMSessionController` with an optional `Runnable onStateChanged` only if incoming messages do not refresh while testing. Set it from `MainActivity`:

```java
session.setOnStateChanged(this::refreshFromImCallback);
```

The controller method should be:

```java
public void setOnStateChanged(Runnable onStateChanged) {
    this.onStateChanged = onStateChanged;
}

private void notifyStateChanged() {
    if (onStateChanged != null) onStateChanged.run();
}
```

Call `notifyStateChanged()` from receive methods and status callbacks after saving.

- [ ] **Step 4: Run Android tests and build**

Run:

```bash
cd android/MultiAIIM
./gradlew testDebugUnitTest assembleDebug
```

Expected: tests pass and debug build succeeds.

- [ ] **Step 5: Install and smoke test on device**

Run:

```bash
cd android/MultiAIIM
./gradlew installDebug
```

Manual expected result:

- App launches.
- Login with a valid IM account ID connects without staying local-only.
- Text message reaches iOS or desktop IM peer.
- Picked image reaches peer.
- Recorded voice reaches peer.
- Incoming text/image/voice from peer appears in Android.

- [ ] **Step 6: Commit**

```bash
git add android/MultiAIIM/app/src/main/java/com/multiaicode/remoteim/MainActivity.java \
  android/MultiAIIM/app/src/main/java/com/multiaicode/remoteim/RemoteIMSessionController.java
git commit -F /tmp/android-im-task5.msg
```

Use this commit message:

```text
OPTIMIZE: 打通 Android IM 真机收发
EFFECTION: Android IM 客户端支持真实文本图片语音收发
TESTPOINT: Android 真机文本图片语音互发验证通过
```

## Final Verification

- [ ] Run Android unit tests:

```bash
cd android/MultiAIIM
./gradlew testDebugUnitTest
```

Expected: all tests pass.

- [ ] Build Android debug package:

```bash
cd android/MultiAIIM
./gradlew assembleDebug
```

Expected: build succeeds.

- [ ] Install on Android device:

```bash
cd android/MultiAIIM
./gradlew installDebug
```

Expected: install succeeds.

- [ ] Manual IM verification:

```text
Android -> iOS/Desktop: text, image, voice
iOS/Desktop -> Android: text, image, voice
Android app restart: local history still present
```
