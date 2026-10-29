// 文件作用：普通 Java PDF 密码保护入口；与 Agent 复用 C++ 核心，不通过工具参数传递密码。
package com.kongshang.maichat.tools;

import android.os.Looper;
import java.io.File;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.CharBuffer;
import java.nio.charset.CharacterCodingException;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import org.json.JSONObject;

/** 在允许文件 I/O 的 worker 线程调用；核心原子发布输出，不生成未保护的最终目标。 */
public final class MaiPdfSecurity {
    private MaiPdfSecurity() {}

    static byte[] strictUtf8(String value) throws CharacterCodingException {
        ByteBuffer buffer = StandardCharsets.UTF_8.newEncoder()
            .onMalformedInput(CodingErrorAction.REPORT).onUnmappableCharacter(CodingErrorAction.REPORT)
            .encode(CharBuffer.wrap(value));
        byte[] bytes = new byte[buffer.remaining()];
        buffer.get(bytes);
        return bytes;
    }

    public static boolean isValidPassword(String value, String confirmation) {
        if (value == null || value.isEmpty() || !value.equals(confirmation) || value.indexOf('\0') >= 0)
            return false;
        try {
            byte[] bytes = strictUtf8(value);
            boolean valid = bytes.length <= 127;
            Arrays.fill(bytes, (byte) 0);
            return valid;
        } catch (CharacterCodingException malformed) { return false; }
    }

    public static JSONObject encrypt(File source, File output, String password,
        boolean replaceExisting) throws Exception {
        if (Looper.myLooper() == Looper.getMainLooper())
            throw new IllegalStateException("PDF encryption requires a worker thread");
        if (source == null || output == null || !source.isAbsolute() || !output.isAbsolute() ||
            source.getPath().indexOf('\0') >= 0 || output.getPath().indexOf('\0') >= 0 ||
            !isValidPassword(password, password)) throw new IOException("Invalid PDF protection input");
        byte[] secret = strictUtf8(password);
        byte[] response;
        try {
            NativeLibrary.ensureLoaded();
            response = nativeEncrypt(strictUtf8(source.getPath()), strictUtf8(output.getPath()),
                secret, replaceExisting);
        } catch (LinkageError unavailable) {
            throw new IOException("PDF encryption component is unavailable");
        } finally {
            // 仅清理本层拥有的临时字节，不保存密码到 SharedPreferences、工具 JSON 或日志。
            Arrays.fill(secret, (byte) 0);
        }
        if (response == null) throw new IOException("PDF encryption failed");
        JSONObject value = new JSONObject(new String(response, StandardCharsets.UTF_8));
        if (!Boolean.TRUE.equals(value.opt("ok"))) throw new IOException("PDF encryption failed");
        JSONObject result = value.optJSONObject("output");
        if (result == null || !Boolean.TRUE.equals(result.opt("password_protected")) ||
            !output.getPath().equals(result.optString("path")))
            throw new IOException("PDF encryption response is invalid");
        return result;
    }

    /** 普通业务也复用同一个本地双密码表单，取消后不调用加密核心。worker 线程调用。 */
    public static JSONObject encryptWithPasswordForm(File source, File output,
        MobilePdfPasswordRequests prompts, boolean replaceExisting) throws Exception {
        String password = prompts.requestPassword();
        if (password == null) throw new java.util.concurrent.CancellationException("PDF password entry canceled");
        if (Thread.currentThread().isInterrupted()) throw new InterruptedException();
        return encrypt(source, output, password, replaceExisting);
    }

    private static final class NativeLibrary {
        static { System.loadLibrary("maichat_agent"); }
        static void ensureLoaded() {}
    }
    // source/output/password 均为真正 UTF-8 字节；不使用 JNI modified UTF-8，不拼接含密码 JSON。
    private static native byte[] nativeEncrypt(byte[] source, byte[] output, byte[] password,
        boolean replaceExisting);
}
