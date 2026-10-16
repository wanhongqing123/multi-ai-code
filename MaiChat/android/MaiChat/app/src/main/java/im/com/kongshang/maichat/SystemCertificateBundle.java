// 文件作用：把 Android 当前信任的根证书导出为原生 HTTPS 客户端可读取的 PEM。
// 账号请求和创作中心共用本实现；每次调用读取当前系统信任配置。
package com.kongshang.maichat;

import android.util.Base64;
import android.util.AtomicFile;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.security.KeyStore;
import java.security.cert.X509Certificate;

import javax.net.ssl.TrustManager;
import javax.net.ssl.TrustManagerFactory;
import javax.net.ssl.X509TrustManager;

final class SystemCertificateBundle {
    private static final int MAX_PEM_BYTES = 2 * 1024 * 1024;
    private static final int MAX_CERTIFICATES = 512;
    private static final byte[] BEGIN =
        "-----BEGIN CERTIFICATE-----\n".getBytes(StandardCharsets.US_ASCII);
    private static final byte[] END =
        "\n-----END CERTIFICATE-----\n".getBytes(StandardCharsets.US_ASCII);

    private SystemCertificateBundle() { }

    static byte[] currentPem() throws Exception {
        TrustManagerFactory factory =
            TrustManagerFactory.getInstance(TrustManagerFactory.getDefaultAlgorithm());
        factory.init((KeyStore) null);
        ByteArrayOutputStream output = new ByteArrayOutputStream();
        int count = 0;
        for (TrustManager manager : factory.getTrustManagers()) {
            if (!(manager instanceof X509TrustManager)) continue;
            for (X509Certificate certificate : ((X509TrustManager) manager).getAcceptedIssuers()) {
                if (++count > MAX_CERTIFICATES)
                    throw new IOException("系统信任证书过多");
                byte[] encoded = Base64.encode(certificate.getEncoded(), Base64.NO_WRAP);
                if (output.size() + BEGIN.length + encoded.length + END.length > MAX_PEM_BYTES)
                    throw new IOException("系统信任证书过大");
                output.write(BEGIN);
                output.write(encoded);
                output.write(END);
            }
        }
        if (count == 0) throw new IOException("无法读取系统信任证书");
        return output.toByteArray();
    }

    static void writeCurrentPem(File destination) throws Exception {
        byte[] pem = currentPem();
        AtomicFile file = new AtomicFile(destination);
        FileOutputStream output = file.startWrite();
        try {
            output.write(pem);
            file.finishWrite(output);
        } catch (Exception failure) {
            file.failWrite(output);
            throw failure;
        }
    }
}
