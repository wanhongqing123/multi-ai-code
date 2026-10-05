#include "MaiSshTool.h"

#if defined(_WIN32)
#include <winsock2.h>
#else
#include <sys/select.h>
#endif

#include <curl/curl.h>
#include <libssh2.h>
#include <openssl/evp.h>
#include <json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include "MaiBlockingCheck.h"

namespace {

using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;

constexpr std::size_t kMaximumOutputBytes = 48 * 1024;

bool validHost(const std::string& host) {
    if (host.empty() || host.size() > 253) return false;
    for (unsigned char ch : host) {
        if (!(std::isalnum(ch) || ch == '.' || ch == '-' || ch == ':' || ch == '_')) return false;
    }
    return true;
}

bool validUsername(const std::string& username) {
    if (username.empty() || username.size() > 128) return false;
    for (unsigned char ch : username) {
        if (!(std::isalnum(ch) || ch == '.' || ch == '-' || ch == '_')) return false;
    }
    return true;
}

std::string tcpUrl(const std::string& host, int port) {
    const std::string address = host.find(':') == std::string::npos ? host : "[" + host + "]";
    return "http://" + address + ":" + std::to_string(port) + "/";
}

MaiError waitSocket(curl_socket_t socket, LIBSSH2_SESSION* session, Clock::time_point deadline,
                    const std::atomic<bool>* cancel) {
    while (true) {
        if (cancel && cancel->load(std::memory_order_relaxed))
            return {MaiErrorCode::Canceled, "SSH command was canceled"};
        const auto now = Clock::now();
        if (now >= deadline) return {MaiErrorCode::Network, "SSH operation timed out"};
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        const long waitMillis = std::min<long>(200, static_cast<long>(remaining));
        const int directions = libssh2_session_block_directions(session);
        fd_set readSet;
        fd_set writeSet;
        FD_ZERO(&readSet);
        FD_ZERO(&writeSet);
        if (directions & LIBSSH2_SESSION_BLOCK_INBOUND) FD_SET(socket, &readSet);
        if (directions & LIBSSH2_SESSION_BLOCK_OUTBOUND) FD_SET(socket, &writeSet);
        if (directions == 0) {
            FD_SET(socket, &readSet);
            FD_SET(socket, &writeSet);
        }
        timeval delay{};
        delay.tv_sec = waitMillis / 1000;
        delay.tv_usec = (waitMillis % 1000) * 1000;
#if defined(_WIN32)
        const int ready = select(0, &readSet, &writeSet, nullptr, &delay);
#else
        const int ready =
            select(static_cast<int>(socket) + 1, &readSet, &writeSet, nullptr, &delay);
#endif
        if (ready > 0) return {};
        if (ready < 0) return {MaiErrorCode::Network, "SSH socket wait failed"};
    }
}

MaiResult<std::string> hostFingerprint(LIBSSH2_SESSION* session) {
    std::size_t length = 0;
    int type = 0;
    const char* key = libssh2_session_hostkey(session, &length, &type);
    if (!key || length == 0) return {MaiErrorCode::Protocol, "SSH server sent no host key"};
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digestLength = 0;
    if (EVP_Digest(key, length, digest.data(), &digestLength, EVP_sha256(), nullptr) != 1)
        return {MaiErrorCode::Internal, "SSH host-key fingerprint failed"};
    std::string encoded(((digestLength + 2) / 3) * 4 + 1, '\0');
    const int count = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(encoded.data()),
                                      digest.data(), static_cast<int>(digestLength));
    if (count <= 0) return {MaiErrorCode::Internal, "SSH fingerprint encoding failed"};
    encoded.resize(static_cast<std::size_t>(count));
    while (!encoded.empty() && encoded.back() == '=') encoded.pop_back();
    return "SHA256:" + encoded;
}

MaiError sshError(LIBSSH2_SESSION* session, const char* stage) {
    return {MaiErrorCode::Network, std::string("SSH ") + stage + " failed (code " +
                                       std::to_string(libssh2_session_last_errno(session)) + ")"};
}

void appendOutput(std::string& output, bool& truncated, const char* data, std::size_t bytes) {
    const std::size_t kept = std::min(bytes, kMaximumOutputBytes - output.size());
    output.append(data, kept);
    if (kept != bytes) truncated = true;
}

class SshTool final : public MaiTool {
public:
    SshTool(MaiSshPasswordProvider passwordProvider, MaiSshHostKeyVerifier verifyHostKey)
        : mPasswordProvider(std::move(passwordProvider)),
          mVerifyHostKey(std::move(verifyHostKey)) {}

    std::string name() const override {
        return "ssh_exec";
    }
    std::string description() const override {
        return "Connect to a remote SSH server and execute one command. The host asks the user "
               "for the password in a secure native dialog; never put a password in arguments "
               "or chat. A new server's SHA-256 fingerprint must be verified before login. "
               "Every remote command needs user approval. This is not an interactive terminal.";
    }
    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{"host":{"type":"string"},"port":{"type":"integer","minimum":1,"maximum":65535},"username":{"type":"string"},"command":{"type":"string"},"timeout_seconds":{"type":"integer","minimum":1,"maximum":120}},"required":["host","username","command"],"additionalProperties":false})";
    }
    bool requiresApproval(const std::string&) const override {
        return true;
    }
    bool requiresPerCallApproval(const std::string&) const override {
        return true;
    }

    MaiToolResult execute(const std::string& raw, const MaiToolContext& context) override {
        const Json args = Json::parse(raw, nullptr, false);
        if (!args.is_object() || !args.value("host", Json{}).is_string() ||
            !args.value("username", Json{}).is_string() ||
            !args.value("command", Json{}).is_string() ||
            (args.contains("port") && !args["port"].is_number_integer()) ||
            (args.contains("timeout_seconds") && !args["timeout_seconds"].is_number_integer()))
            return MaiToolResult::failure(MaiErrorCode::InvalidInput, "invalid SSH arguments");
        MaiSshExecRequest request;
        request.host = args["host"].get<std::string>();
        request.port = args.value("port", 22);
        request.username = args["username"].get<std::string>();
        request.command = args["command"].get<std::string>();
        request.timeoutSeconds = args.value("timeout_seconds", 30);
        if (!validHost(request.host) || !validUsername(request.username) ||
            request.command.empty() || request.command.size() > 8192 || request.port < 1 ||
            request.port > 65535 || request.timeoutSeconds < 1 || request.timeoutSeconds > 120)
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "invalid SSH host or command");
        if (!mPasswordProvider || !mVerifyHostKey)
            return MaiToolResult::failure(MaiErrorCode::NotConfigured,
                                          "SSH host credential and fingerprint UI are unavailable");
        const auto password = mPasswordProvider(request.host, request.port, request.username);
        if (!password)
            return MaiToolResult::failure(password.error().code(), password.error().message());
        request.password = password.value();
        const auto result = maiExecuteSsh(request, mVerifyHostKey, context.cancel);
        if (!result) return MaiToolResult::failure(result.error().code(), result.error().message());
        Json output = {{"host", request.host},
                       {"port", request.port},
                       {"username", request.username},
                       {"stdout", result.value().stdoutText},
                       {"stderr", result.value().stderrText},
                       {"host_fingerprint", result.value().hostFingerprint},
                       {"timed_out", result.value().timedOut},
                       {"truncated", result.value().truncated}};
        if (result.value().hasExitCode) output["exit_code"] = result.value().exitCode;
        return MaiToolResult::success(output.dump(-1, ' ', false, Json::error_handler_t::replace),
                                      result.value().truncated);
    }

private:
    MaiSshPasswordProvider mPasswordProvider;
    MaiSshHostKeyVerifier mVerifyHostKey;
};

}  // namespace

MaiResult<MaiSshExecResult> maiExecuteSsh(const MaiSshExecRequest& request,
                                          const MaiSshHostKeyVerifier& verifyHostKey,
                                          const std::atomic<bool>* cancel) {
    if (!validHost(request.host) || !validUsername(request.username) ||
        request.password.size() > 4096 || request.command.empty() ||
        request.command.size() > 8192 || request.port < 1 || request.port > 65535 ||
        request.timeoutSeconds < 1 || request.timeoutSeconds > 120 || !verifyHostKey)
        return {MaiErrorCode::InvalidInput, "invalid SSH request"};
    static std::once_flag initialized;
    static int initializationResult = -1;
    std::call_once(initialized, [] { initializationResult = libssh2_init(0); });
    if (initializationResult != 0) return {MaiErrorCode::Internal, "libssh2 initialization failed"};
    maiAssertBlockingAllowed("ssh_exec");
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> connection(curl_easy_init(),
                                                                   curl_easy_cleanup);
    if (!connection) return {MaiErrorCode::Internal, "SSH TCP initialization failed"};
    const std::string url = tcpUrl(request.host, request.port);
    curl_easy_setopt(connection.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(connection.get(), CURLOPT_CONNECT_ONLY, 1L);
    curl_easy_setopt(connection.get(), CURLOPT_CONNECTTIMEOUT_MS, 10000L);
    curl_easy_setopt(connection.get(), CURLOPT_TIMEOUT_MS, 10000L);
    curl_easy_setopt(connection.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(connection.get(), CURLOPT_PROXY, "");
    curl_easy_setopt(connection.get(), CURLOPT_NOPROXY, "*");
    curl_easy_setopt(connection.get(), CURLOPT_PROTOCOLS_STR, "http");
    if (curl_easy_perform(connection.get()) != CURLE_OK)
        return {MaiErrorCode::Network, "cannot connect to SSH host"};
    curl_socket_t socket = CURL_SOCKET_BAD;
    if (curl_easy_getinfo(connection.get(), CURLINFO_ACTIVESOCKET, &socket) != CURLE_OK ||
        socket == CURL_SOCKET_BAD)
        return {MaiErrorCode::Network, "SSH TCP socket is unavailable"};

    LIBSSH2_SESSION* rawSession = libssh2_session_init();
    std::unique_ptr<LIBSSH2_SESSION, decltype(&libssh2_session_free)> session(rawSession,
                                                                              libssh2_session_free);
    if (!session) return {MaiErrorCode::Internal, "SSH session initialization failed"};
    libssh2_session_set_blocking(session.get(), 0);
    const auto handshakeDeadline = Clock::now() + std::chrono::seconds(10);
    int code = 0;
    while ((code = libssh2_session_handshake(session.get(), socket)) == LIBSSH2_ERROR_EAGAIN) {
        const MaiError wait = waitSocket(socket, session.get(), handshakeDeadline, cancel);
        if (wait.hasError()) return wait;
    }
    if (code != 0) return sshError(session.get(), "handshake");
    const auto fingerprint = hostFingerprint(session.get());
    if (!fingerprint) return fingerprint.error();
    const auto trusted = verifyHostKey(request.host, request.port, fingerprint.value());
    if (!trusted) return trusted.error();
    if (!trusted.value())
        return {MaiErrorCode::Protocol, "SSH host fingerprint was rejected or changed"};

    const auto authDeadline = Clock::now() + std::chrono::seconds(10);
    while ((code = libssh2_userauth_password_ex(
                session.get(), request.username.c_str(),
                static_cast<unsigned int>(request.username.size()), request.password.c_str(),
                static_cast<unsigned int>(request.password.size()), nullptr)) ==
           LIBSSH2_ERROR_EAGAIN) {
        const MaiError wait = waitSocket(socket, session.get(), authDeadline, cancel);
        if (wait.hasError()) return wait;
    }
    if (code != 0) return {MaiErrorCode::NotConfigured, "SSH username or password was rejected"};

    const auto deadline = Clock::now() + std::chrono::seconds(request.timeoutSeconds);
    LIBSSH2_CHANNEL* rawChannel = nullptr;
    while (!(rawChannel = libssh2_channel_open_session(session.get()))) {
        if (libssh2_session_last_errno(session.get()) != LIBSSH2_ERROR_EAGAIN)
            return sshError(session.get(), "channel open");
        const MaiError wait = waitSocket(socket, session.get(), deadline, cancel);
        if (wait.hasError()) return wait;
    }
    const auto releaseChannel = [sessionPtr = session.get()](LIBSSH2_CHANNEL* channel) {
        libssh2_session_set_blocking(sessionPtr, 1);
        libssh2_session_set_timeout(sessionPtr, 1000);
        libssh2_channel_free(channel);
    };
    std::unique_ptr<LIBSSH2_CHANNEL, decltype(releaseChannel)> channel(rawChannel, releaseChannel);
    while ((code = libssh2_channel_exec(channel.get(), request.command.c_str())) ==
           LIBSSH2_ERROR_EAGAIN) {
        const MaiError wait = waitSocket(socket, session.get(), deadline, cancel);
        if (wait.hasError()) return wait;
    }
    if (code != 0) return sshError(session.get(), "command start");

    MaiSshExecResult result;
    result.hostFingerprint = fingerprint.value();
    std::array<char, 8192> buffer{};
    while (true) {
        if (cancel && cancel->load(std::memory_order_relaxed))
            return {MaiErrorCode::Canceled, "SSH command was canceled"};
        if (Clock::now() >= deadline) {
            result.timedOut = true;
            break;
        }
        bool received = false;
        const auto stdoutBytes = libssh2_channel_read(channel.get(), buffer.data(), buffer.size());
        if (stdoutBytes > 0) {
            appendOutput(result.stdoutText, result.truncated, buffer.data(),
                         static_cast<std::size_t>(stdoutBytes));
            received = true;
        } else if (stdoutBytes < 0 && stdoutBytes != LIBSSH2_ERROR_EAGAIN) {
            return sshError(session.get(), "stdout read");
        }
        const auto stderrBytes =
            libssh2_channel_read_stderr(channel.get(), buffer.data(), buffer.size());
        if (stderrBytes > 0) {
            appendOutput(result.stderrText, result.truncated, buffer.data(),
                         static_cast<std::size_t>(stderrBytes));
            received = true;
        } else if (stderrBytes < 0 && stderrBytes != LIBSSH2_ERROR_EAGAIN) {
            return sshError(session.get(), "stderr read");
        }
        if (libssh2_channel_eof(channel.get()) && !received) break;
        if (!received) {
            const MaiError wait = waitSocket(socket, session.get(), deadline, cancel);
            if (wait.code() == MaiErrorCode::Network && Clock::now() >= deadline) {
                result.timedOut = true;
                break;
            }
            if (wait.hasError()) return wait;
        }
    }
    if (!result.timedOut) {
        result.exitCode = libssh2_channel_get_exit_status(channel.get());
        result.hasExitCode = true;
    }
    return result;
}

std::unique_ptr<MaiTool> makeMaiSshTool(MaiSshPasswordProvider passwordProvider,
                                        MaiSshHostKeyVerifier verifyHostKey) {
    return std::make_unique<SshTool>(std::move(passwordProvider), std::move(verifyHostKey));
}
