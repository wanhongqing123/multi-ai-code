#pragma once

#include <memory>
#include <string>

#include "MaiError.h"
#include "MaiTool.h"

// Hashes bytes with a supported digest algorithm using OpenSSL EVP. The result is lowercase
// hexadecimal. No secrets, file access, or model state are involved. Invalid algorithms and
// OpenSSL failures return MaiError rather than throwing across a platform boundary.
MaiResult<std::string> maiOpenSslDigestHex(const std::string& bytes, const std::string& algorithm);

// Read-only model tools. The digest tool accepts a workspace file or short text; the certificate
// tool inspects one PEM/DER X.509 file. Both resolve paths through MaiToolContext and bound reads.
// Mobile builds use the pinned vendored OpenSSL libcrypto; desktop builds link OpenSSL::Crypto.
std::unique_ptr<MaiTool> makeMaiOpenSslDigestTool();
std::unique_ptr<MaiTool> makeMaiOpenSslCertificateTool();
