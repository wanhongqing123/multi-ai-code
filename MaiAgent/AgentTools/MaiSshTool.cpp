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
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include "MaiBlockingCheck.h"

namespace {

using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;

constexpr std::size_t kMaximumOutputBytes = 48 * 1024;
constexpr auto kConnectionIdleLimit = std::chrono::minutes(10);
constexpr std::size_t kMaximumConnections = 4;

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

struct SshConnection {
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> transport{nullptr, curl_easy_cleanup};
    curl_socket_t socket = CURL_SOCKET_BAD;
    std::unique_ptr<LIBSSH2_SESSION, decltype(&libssh2_session_free)> session{nullptr,
                                                                              libssh2_session_free};
    std::string fingerprint;
    Clock::time_point lastUsed = Clock::now();
};

MaiResult<std::unique_ptr<SshConnection>> openSshConnection(
    const MaiSshExecRequest& request, const MaiSshHostKeyVerifier& verifyHostKey,
    const std::atomic<bool>* cancel);
MaiResult<MaiSshExecResult> executeSshCommand(SshConnection& connection,
                                              const MaiSshExecRequest& request,
                                              const std::atomic<bool>* cancel);

std::string connectionKey(const MaiSshExecRequest& request, const MaiToolContext& context) {
    return context.sessionId + '\0' + request.host + '\0' + std::to_string(request.port) + '\0' +
           request.username;
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
        return "Connect to a remote SSH server and execute one command. A live SSH connection "
               "is reused within this AI conversation for up to ten idle minutes; each command "
               "still runs in a separate remote channel, so cd and shell variables do not "
               "persist. The host asks for the password in a secure native dialog; never put "
               "a password in arguments or chat. A new server's SHA-256 fingerprint must be "
               "verified before login. Every remote command needs user approval.";
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
        const std::string key = connectionKey(request, context);
        std::lock_guard<std::mutex> lock(mMutex);
        auto found = mConnections.find(key);
        if (found != mConnections.end() &&
            Clock::now() - found->second->lastUsed > kConnectionIdleLimit) {
            mConnections.erase(found);
            found = mConnections.end();
        }
        const bool reused = found != mConnections.end();
        if (!reused) {
            const auto password = mPasswordProvider(request.host, request.port, request.username);
            if (!password)
                return MaiToolResult::failure(password.error().code(), password.error().message());
            request.password = password.value();
            if (request.password.size() > 4096) {
                std::fill(request.password.begin(), request.password.end(), '\0');
                return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                              "SSH password exceeds the supported limit");
            }
            auto opened = openSshConnection(request, mVerifyHostKey, context.cancel);
            std::fill(request.password.begin(), request.password.end(), '\0');
            request.password.clear();
            if (!opened)
                return MaiToolResult::failure(opened.error().code(), opened.error().message());
            if (mConnections.size() >= kMaximumConnections) {
                const auto oldest =
                    std::min_element(mConnections.begin(), mConnections.end(),
                                     [](const auto& left, const auto& right) {
                                         return left.second->lastUsed < right.second->lastUsed;
                                     });
                mConnections.erase(oldest);
            }
            found = mConnections.emplace(key, std::move(opened.value())).first;
        }
        const auto result = executeSshCommand(*found->second, request, context.cancel);
        if (!result) {
            mConnections.erase(found);
            return MaiToolResult::failure(result.error().code(), result.error().message());
        }
        if (result.value().timedOut)
            mConnections.erase(found);
        else
            found->second->lastUsed = Clock::now();
        Json output = {{"host", request.host},
                       {"port", request.port},
                       {"username", request.username},
                       {"stdout", result.value().stdoutText},
                       {"stderr", result.value().stderrText},
                       {"host_fingerprint", result.value().hostFingerprint},
                       {"connection_reused", reused},
                       {"timed_out", result.value().timedOut},
                       {"truncated", result.value().truncated}};
        if (result.value().hasExitCode) output["exit_code"] = result.value().exitCode;
        return MaiToolResult::success(output.dump(-1, ' ', false, Json::error_handler_t::replace),
                                      result.value().truncated);
    }

private:
    MaiSshPasswordProvider mPasswordProvider;
    MaiSshHostKeyVerifier mVerifyHostKey;
    std::mutex mMutex;
    std::map<std::string, std::unique_ptr<SshConnection>> mConnections;
};

MaiResult<std::unique_ptr<SshConnection>> openSshConnection(
    const MaiSshExecRequest& request, const MaiSshHostKeyVerifier& verifyHostKey,
    const std::atomic<bool>* cancel) {
    static std::once_flag initialized;
    static int initializationResult = -1;
    std::call_once(initialized, [] { initializationResult = libssh2_init(0); });
    if (initializationResult != 0) return {MaiErrorCode::Internal, "libssh2 initialization failed"};
    maiAssertBlockingAllowed("ssh_exec");
    auto connection = std::make_unique<SshConnection>();
    connection->transport.reset(curl_easy_init());
    if (!connection->transport) return {MaiErrorCode::Internal, "SSH TCP initialization failed"};
    const std::string url = tcpUrl(request.host, request.port);
    curl_easy_setopt(connection->transport.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(connection->transport.get(), CURLOPT_CONNECT_ONLY, 1L);
    curl_easy_setopt(connection->transport.get(), CURLOPT_CONNECTTIMEOUT_MS, 10000L);
    curl_easy_setopt(connection->transport.get(), CURLOPT_TIMEOUT_MS, 10000L);
    curl_easy_setopt(connection->transport.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(connection->transport.get(), CURLOPT_PROXY, "");
    curl_easy_setopt(connection->transport.get(), CURLOPT_NOPROXY, "*");
    curl_easy_setopt(connection->transport.get(), CURLOPT_PROTOCOLS_STR, "http");
    if (curl_easy_perform(connection->transport.get()) != CURLE_OK)
        return {MaiErrorCode::Network, "cannot connect to SSH host"};
    if (curl_easy_getinfo(connection->transport.get(), CURLINFO_ACTIVESOCKET,
                          &connection->socket) != CURLE_OK ||
        connection->socket == CURL_SOCKET_BAD)
        return {MaiErrorCode::Network, "SSH TCP socket is unavailable"};

    connection->session.reset(libssh2_session_init());
    if (!connection->session) return {MaiErrorCode::Internal, "SSH session initialization failed"};
    libssh2_session_set_blocking(connection->session.get(), 0);
    const auto handshakeDeadline = Clock::now() + std::chrono::seconds(10);
    int code = 0;
    while ((code = libssh2_session_handshake(connection->session.get(), connection->socket)) ==
           LIBSSH2_ERROR_EAGAIN) {
        const MaiError wait =
            waitSocket(connection->socket, connection->session.get(), handshakeDeadline, cancel);
        if (wait.hasError()) return wait;
    }
    if (code != 0) return sshError(connection->session.get(), "handshake");
    const auto fingerprint = hostFingerprint(connection->session.get());
    if (!fingerprint) return fingerprint.error();
    const auto trusted = verifyHostKey(request.host, request.port, fingerprint.value());
    if (!trusted) return trusted.error();
    if (!trusted.value())
        return {MaiErrorCode::Protocol, "SSH host fingerprint was rejected or changed"};

    const auto authDeadline = Clock::now() + std::chrono::seconds(10);
    while ((code = libssh2_userauth_password_ex(
                connection->session.get(), request.username.c_str(),
                static_cast<unsigned int>(request.username.size()), request.password.c_str(),
                static_cast<unsigned int>(request.password.size()), nullptr)) ==
           LIBSSH2_ERROR_EAGAIN) {
        const MaiError wait =
            waitSocket(connection->socket, connection->session.get(), authDeadline, cancel);
        if (wait.hasError()) return wait;
    }
    if (code != 0) return {MaiErrorCode::NotConfigured, "SSH username or password was rejected"};
    connection->fingerprint = fingerprint.value();
    connection->lastUsed = Clock::now();
    return connection;
}

MaiResult<MaiSshExecResult> executeSshCommand(SshConnection& connection,
                                              const MaiSshExecRequest& request,
                                              const std::atomic<bool>* cancel) {
    LIBSSH2_SESSION* session = connection.session.get();
    const curl_socket_t socket = connection.socket;
    libssh2_session_set_blocking(session, 0);
    const auto deadline = Clock::now() + std::chrono::seconds(request.timeoutSeconds);
    LIBSSH2_CHANNEL* rawChannel = nullptr;
    while (!(rawChannel = libssh2_channel_open_session(session))) {
        if (libssh2_session_last_errno(session) != LIBSSH2_ERROR_EAGAIN)
            return sshError(session, "channel open");
        const MaiError wait = waitSocket(socket, session, deadline, cancel);
        if (wait.hasError()) return wait;
    }
    const auto releaseChannel = [session](LIBSSH2_CHANNEL* channel) {
        libssh2_session_set_blocking(session, 1);
        libssh2_session_set_timeout(session, 1000);
        libssh2_channel_free(channel);
        libssh2_session_set_blocking(session, 0);
    };
    std::unique_ptr<LIBSSH2_CHANNEL, decltype(releaseChannel)> channel(rawChannel, releaseChannel);
    int code = 0;
    while ((code = libssh2_channel_exec(channel.get(), request.command.c_str())) ==
           LIBSSH2_ERROR_EAGAIN) {
        const MaiError wait = waitSocket(socket, session, deadline, cancel);
        if (wait.hasError()) return wait;
    }
    if (code != 0) return sshError(session, "command start");

    MaiSshExecResult result;
    result.hostFingerprint = connection.fingerprint;
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
            return sshError(session, "stdout read");
        }
        const auto stderrBytes =
            libssh2_channel_read_stderr(channel.get(), buffer.data(), buffer.size());
        if (stderrBytes > 0) {
            appendOutput(result.stderrText, result.truncated, buffer.data(),
                         static_cast<std::size_t>(stderrBytes));
            received = true;
        } else if (stderrBytes < 0 && stderrBytes != LIBSSH2_ERROR_EAGAIN) {
            return sshError(session, "stderr read");
        }
        if (libssh2_channel_eof(channel.get()) && !received) break;
        if (!received) {
            const MaiError wait = waitSocket(socket, session, deadline, cancel);
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

}  // namespace

MaiResult<MaiSshExecResult> maiExecuteSsh(const MaiSshExecRequest& request,
                                          const MaiSshHostKeyVerifier& verifyHostKey,
                                          const std::atomic<bool>* cancel) {
    if (!validHost(request.host) || !validUsername(request.username) ||
        request.password.size() > 4096 || request.command.empty() ||
        request.command.size() > 8192 || request.port < 1 || request.port > 65535 ||
        request.timeoutSeconds < 1 || request.timeoutSeconds > 120 || !verifyHostKey)
        return {MaiErrorCode::InvalidInput, "invalid SSH request"};
    auto connection = openSshConnection(request, verifyHostKey, cancel);
    if (!connection) return connection.error();
    return executeSshCommand(*connection.value(), request, cancel);
}

std::unique_ptr<MaiTool> makeMaiSshTool(MaiSshPasswordProvider passwordProvider,
                                        MaiSshHostKeyVerifier verifyHostKey) {
    return std::make_unique<SshTool>(std::move(passwordProvider), std::move(verifyHostKey));
}
