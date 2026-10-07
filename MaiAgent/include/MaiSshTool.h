#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>

#include "MaiError.h"
#include "MaiTool.h"

// A single non-interactive SSH command. The password is transient host input and is never part
// of the model tool schema or persisted tool arguments. The caller owns host-key trust policy:
// validation receives the standard SHA256:base64 fingerprint before authentication starts.
struct MaiSshExecRequest {
    std::string host;
    int port = 22;
    std::string username;
    std::string password;
    std::string command;
    int timeoutSeconds = 30;
};

struct MaiSshExecResult {
    std::string stdoutText;
    std::string stderrText;
    std::string hostFingerprint;
    int exitCode = 0;
    bool hasExitCode = false;
    bool timedOut = false;
    bool truncated = false;
};

using MaiSshPasswordProvider = std::function<MaiResult<std::string>(
    const std::string& host, int port, const std::string& username)>;
using MaiSshHostKeyVerifier = std::function<MaiResult<bool>(const std::string& host, int port,
                                                            const std::string& fingerprint)>;

// Shared C++ transport for iOS, Android, Mac, PC, and CLI. It uses libssh2 over a direct TCP
// connection supplied by libcurl, bounds stdout/stderr independently to 48 KiB, and checks
// cancellation during network waits. A timeout returns the partial output with timedOut=true;
// connection, authentication, and host-key errors return MaiError. Safe for concurrent calls.
MaiResult<MaiSshExecResult> maiExecuteSsh(const MaiSshExecRequest& request,
                                          const MaiSshHostKeyVerifier& verifyHostKey,
                                          const std::atomic<bool>* cancel = nullptr);

// Model tool wrapper. Both callbacks must be backed by a secure host UI or credential store.
// Missing callbacks fail closed. The tool reuses a live authenticated connection within the
// same Agent conversation for commands submitted less than ten minutes apart; commands use
// separate SSH channels and do not share shell state. The connection is not persisted across
// process restarts. Every remote command requires per-call approval.
std::unique_ptr<MaiTool> makeMaiSshTool(MaiSshPasswordProvider passwordProvider,
                                        MaiSshHostKeyVerifier verifyHostKey);
