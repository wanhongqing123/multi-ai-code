#pragma once

#include <memory>
#include <string>
#include <vector>

#include "MaiTool.h"

// Runs the audited read-only subset of the embedded OpenSSL command line. Calls are serialized
// because upstream apps use process-global BIOs and option state. Arguments omit "openssl".
// Input files must resolve inside the tool context and cannot exceed the per-command limit.
// The result contains the actual command exit code and bounded stdout/stderr. Unsupported
// subcommands and options fail before calling upstream code.
struct MaiOpenSslCommandResult {
    int exitCode = 0;
    std::string standardOutput;
    std::string errorOutput;
    bool truncated = false;
};

MaiResult<MaiOpenSslCommandResult> maiRunOpenSslCommand(
    const std::vector<std::string>& arguments, const MaiToolContext& context);

std::unique_ptr<MaiTool> makeMaiOpenSslCliTool();
