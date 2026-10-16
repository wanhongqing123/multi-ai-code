// 文件作用：只供 MaiChat 账号域名的早期 TLS 握手回退使用。
// 标准连接遭遇经证实的 SNI 相关重置后，此工厂沿用该连接原有的 TLS 信任链，
// 连接系统 DNS 已选定的同一 IP，但不在 ClientHello 中发送域名 SNI。
// URL 仍是 ichat.life，调用方继续使用 HttpsURLConnection 原有的域名校验器。
// 如果日后配置 NetworkSecurityConfig 证书固定，必须重新核验这条默认信任链路径。
package com.kongshang.maichat;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.InetAddress;
import java.net.Socket;
import java.net.SocketAddress;
import java.net.SocketException;
import java.nio.channels.SocketChannel;
import java.util.Collections;
import java.util.List;
import java.util.function.BiFunction;

import javax.net.ssl.HandshakeCompletedListener;
import javax.net.ssl.SSLParameters;
import javax.net.ssl.SSLSession;
import javax.net.ssl.SSLSocket;
import javax.net.ssl.SSLSocketFactory;

final class AuthenticationNoSniSocketFactory extends SSLSocketFactory {
    private final SSLSocketFactory connectionFactory;

    AuthenticationNoSniSocketFactory(SSLSocketFactory connectionFactory) {
        this.connectionFactory = connectionFactory;
    }

    private Socket omitServerName(Socket secureSocket) {
        return new NoSniSocket((SSLSocket) secureSocket);
    }

    private InetAddress ipAddressWithoutHostname(InetAddress resolvedAddress) throws IOException {
        // 直连重载也必须丢弃 InetAddress 内保存的域名，否则部分 TLS 提供者会重新生成 SNI。
        return InetAddress.getByAddress(resolvedAddress.getAddress());
    }

    @Override public String[] getDefaultCipherSuites() {
        return connectionFactory.getDefaultCipherSuites();
    }

    @Override public String[] getSupportedCipherSuites() {
        return connectionFactory.getSupportedCipherSuites();
    }

    @Override public Socket createSocket(Socket connectedSocket, String host, int port,
        boolean closeConnectedSocket) throws IOException {
        InetAddress connectedAddress = connectedSocket.getInetAddress();
        if (connectedAddress == null) throw new SocketException("TLS peer is not connected");
        // Android Conscrypt 在创建 TLS socket 时会记住 host；仅清空 SSLParameters 无法去掉 SNI。
        // 使用当前 TCP 连接已选定的 IP，保留原 DNS/路由，且避免在 TLS 层重新写入域名。
        return omitServerName(connectionFactory.createSocket(connectedSocket,
            connectedAddress.getHostAddress(), port, closeConnectedSocket));
    }

    @Override public Socket createSocket(String host, int port) throws IOException {
        return omitServerName(connectionFactory.createSocket(
            ipAddressWithoutHostname(InetAddress.getByName(host)), port));
    }

    @Override public Socket createSocket(String host, int port, InetAddress localAddress,
        int localPort) throws IOException {
        return omitServerName(connectionFactory.createSocket(
            ipAddressWithoutHostname(InetAddress.getByName(host)), port, localAddress, localPort));
    }

    @Override public Socket createSocket(InetAddress address, int port) throws IOException {
        return omitServerName(connectionFactory.createSocket(
            ipAddressWithoutHostname(address), port));
    }

    @Override public Socket createSocket(InetAddress address, int port,
        InetAddress localAddress, int localPort) throws IOException {
        return omitServerName(connectionFactory.createSocket(
            ipAddressWithoutHostname(address), port, localAddress, localPort));
    }

    @Override public Socket createSocket() throws IOException {
        return omitServerName(connectionFactory.createSocket());
    }

    private static final class NoSniSocket extends SSLSocket {
        private final SSLSocket systemSocket;

        NoSniSocket(SSLSocket systemSocket) {
            this.systemSocket = systemSocket;
            clearServerNames();
        }

        private void clearServerNames() {
            SSLParameters parameters = systemSocket.getSSLParameters();
            parameters.setServerNames(Collections.emptyList());
            systemSocket.setSSLParameters(parameters);
        }

        @Override public void startHandshake() throws IOException {
            clearServerNames();
            systemSocket.startHandshake();
        }

        @Override public SSLParameters getSSLParameters() {
            return systemSocket.getSSLParameters();
        }

        @Override public void setSSLParameters(SSLParameters parameters) {
            parameters.setServerNames(Collections.emptyList());
            systemSocket.setSSLParameters(parameters);
        }

        @Override public String[] getSupportedCipherSuites() {
            return systemSocket.getSupportedCipherSuites();
        }

        @Override public String[] getEnabledCipherSuites() {
            return systemSocket.getEnabledCipherSuites();
        }

        @Override public void setEnabledCipherSuites(String[] cipherSuites) {
            systemSocket.setEnabledCipherSuites(cipherSuites);
        }

        @Override public String[] getSupportedProtocols() {
            return systemSocket.getSupportedProtocols();
        }

        @Override public String[] getEnabledProtocols() {
            return systemSocket.getEnabledProtocols();
        }

        @Override public void setEnabledProtocols(String[] protocols) {
            systemSocket.setEnabledProtocols(protocols);
        }

        @Override public SSLSession getSession() { return systemSocket.getSession(); }

        @Override public SSLSession getHandshakeSession() {
            return systemSocket.getHandshakeSession();
        }

        @Override public String getApplicationProtocol() {
            return systemSocket.getApplicationProtocol();
        }

        @Override public String getHandshakeApplicationProtocol() {
            return systemSocket.getHandshakeApplicationProtocol();
        }

        @Override public void setHandshakeApplicationProtocolSelector(
            BiFunction<SSLSocket, List<String>, String> selector) {
            systemSocket.setHandshakeApplicationProtocolSelector(selector);
        }

        @Override public BiFunction<SSLSocket, List<String>, String>
            getHandshakeApplicationProtocolSelector() {
            return systemSocket.getHandshakeApplicationProtocolSelector();
        }

        @Override public void addHandshakeCompletedListener(HandshakeCompletedListener listener) {
            systemSocket.addHandshakeCompletedListener(listener);
        }

        @Override public void removeHandshakeCompletedListener(HandshakeCompletedListener listener) {
            systemSocket.removeHandshakeCompletedListener(listener);
        }

        @Override public void setUseClientMode(boolean useClientMode) {
            systemSocket.setUseClientMode(useClientMode);
        }

        @Override public boolean getUseClientMode() { return systemSocket.getUseClientMode(); }

        @Override public void setNeedClientAuth(boolean needClientAuth) {
            systemSocket.setNeedClientAuth(needClientAuth);
        }

        @Override public boolean getNeedClientAuth() { return systemSocket.getNeedClientAuth(); }

        @Override public void setWantClientAuth(boolean wantClientAuth) {
            systemSocket.setWantClientAuth(wantClientAuth);
        }

        @Override public boolean getWantClientAuth() { return systemSocket.getWantClientAuth(); }

        @Override public void setEnableSessionCreation(boolean enableSessionCreation) {
            systemSocket.setEnableSessionCreation(enableSessionCreation);
        }

        @Override public boolean getEnableSessionCreation() {
            return systemSocket.getEnableSessionCreation();
        }

        @Override public InputStream getInputStream() throws IOException {
            return systemSocket.getInputStream();
        }

        @Override public OutputStream getOutputStream() throws IOException {
            return systemSocket.getOutputStream();
        }

        @Override public void close() throws IOException { systemSocket.close(); }

        @Override public void connect(SocketAddress address) throws IOException {
            systemSocket.connect(address);
        }

        @Override public void connect(SocketAddress address, int timeout) throws IOException {
            systemSocket.connect(address, timeout);
        }

        @Override public void bind(SocketAddress address) throws IOException {
            systemSocket.bind(address);
        }

        @Override public InetAddress getInetAddress() { return systemSocket.getInetAddress(); }

        @Override public InetAddress getLocalAddress() { return systemSocket.getLocalAddress(); }

        @Override public int getPort() { return systemSocket.getPort(); }

        @Override public int getLocalPort() { return systemSocket.getLocalPort(); }

        @Override public SocketAddress getRemoteSocketAddress() {
            return systemSocket.getRemoteSocketAddress();
        }

        @Override public SocketAddress getLocalSocketAddress() {
            return systemSocket.getLocalSocketAddress();
        }

        @Override public SocketChannel getChannel() { return systemSocket.getChannel(); }

        @Override public void setSoTimeout(int timeout) throws SocketException {
            systemSocket.setSoTimeout(timeout);
        }

        @Override public int getSoTimeout() throws SocketException {
            return systemSocket.getSoTimeout();
        }

        @Override public void setTcpNoDelay(boolean enabled) throws SocketException {
            systemSocket.setTcpNoDelay(enabled);
        }

        @Override public boolean getTcpNoDelay() throws SocketException {
            return systemSocket.getTcpNoDelay();
        }

        @Override public void setKeepAlive(boolean enabled) throws SocketException {
            systemSocket.setKeepAlive(enabled);
        }

        @Override public boolean getKeepAlive() throws SocketException {
            return systemSocket.getKeepAlive();
        }

        @Override public void setReuseAddress(boolean enabled) throws SocketException {
            systemSocket.setReuseAddress(enabled);
        }

        @Override public boolean getReuseAddress() throws SocketException {
            return systemSocket.getReuseAddress();
        }

        @Override public void setReceiveBufferSize(int size) throws SocketException {
            systemSocket.setReceiveBufferSize(size);
        }

        @Override public int getReceiveBufferSize() throws SocketException {
            return systemSocket.getReceiveBufferSize();
        }

        @Override public void setSendBufferSize(int size) throws SocketException {
            systemSocket.setSendBufferSize(size);
        }

        @Override public int getSendBufferSize() throws SocketException {
            return systemSocket.getSendBufferSize();
        }

        @Override public void setSoLinger(boolean enabled, int linger) throws SocketException {
            systemSocket.setSoLinger(enabled, linger);
        }

        @Override public int getSoLinger() throws SocketException {
            return systemSocket.getSoLinger();
        }

        @Override public void setTrafficClass(int trafficClass) throws SocketException {
            systemSocket.setTrafficClass(trafficClass);
        }

        @Override public int getTrafficClass() throws SocketException {
            return systemSocket.getTrafficClass();
        }

        @Override public boolean isConnected() { return systemSocket.isConnected(); }

        @Override public boolean isBound() { return systemSocket.isBound(); }

        @Override public boolean isClosed() { return systemSocket.isClosed(); }

        @Override public boolean isInputShutdown() { return systemSocket.isInputShutdown(); }

        @Override public boolean isOutputShutdown() { return systemSocket.isOutputShutdown(); }

        @Override public void shutdownInput() throws IOException { systemSocket.shutdownInput(); }

        @Override public void shutdownOutput() throws IOException { systemSocket.shutdownOutput(); }
    }
}
