// 文件作用：验证原生 PDF 安全输入与 C++ 核心相同的 UTF-8/NUL/127 字节边界，不加载 JNI。
package com.kongshang.maichat.tools;

import static org.junit.Assert.*;
import org.junit.Test;

public class MaiPdfSecurityTest {
    @Test public void requiresNonemptyMatchingPasswordsWithoutTrimming() {
        assertFalse(MaiPdfSecurity.isValidPassword("", ""));
        assertFalse(MaiPdfSecurity.isValidPassword(null, null));
        assertFalse(MaiPdfSecurity.isValidPassword("secret", "secret "));
        assertTrue(MaiPdfSecurity.isValidPassword(" ", " "));
        assertTrue(MaiPdfSecurity.isValidPassword(" secret ", " secret "));
    }

    @Test public void enforcesUtf8BytesRatherThanCharacters() {
        String boundary = String.join("", java.util.Collections.nCopies(127, "a"));
        assertTrue(MaiPdfSecurity.isValidPassword(boundary, boundary));
        assertFalse(MaiPdfSecurity.isValidPassword(boundary + "a", boundary + "a"));
        String emoji = new String(Character.toChars(0x1f642));
        String valid = String.join("", java.util.Collections.nCopies(31, emoji)) + "abc";
        assertTrue(MaiPdfSecurity.isValidPassword(valid, valid));
        assertFalse(MaiPdfSecurity.isValidPassword(valid + "d", valid + "d"));
    }

    @Test public void rejectsEmbeddedNulAndMalformedUtf16WithoutReplacement() {
        assertFalse(MaiPdfSecurity.isValidPassword("a\0b", "a\0b"));
        assertFalse(MaiPdfSecurity.isValidPassword("bad\ud800", "bad\ud800"));
        assertFalse(MaiPdfSecurity.isValidPassword("bad\udc00", "bad\udc00"));
    }
}
