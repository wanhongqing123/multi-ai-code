#pragma once

#include <cstddef>
#include <memory>
#include <string>

#include "MaiError.h"
#include "MaiTool.h"

// Compresses or decompresses in-memory bytes using a selected zlib stream format. "auto" is
// accepted only for decompression and recognizes gzip and zlib headers. Inputs and outputs are
// bounded by the caller; exceeding maxOutputBytes returns InvalidInput and no partial result.
// Safe to call concurrently. No file access, global state, or model state is involved.
MaiResult<std::string> maiTransformZlibBytes(const std::string& input, bool compress,
                                             const std::string& format, std::size_t maxOutputBytes);

// Both tools read an existing workspace file and create a new file without overwriting it.
// They use the same byte transform above. Paths are resolved through MaiToolContext, so they
// cannot escape the host's configured file boundary. File mutations require user approval.
std::unique_ptr<MaiTool> makeMaiZlibCompressTool();
std::unique_ptr<MaiTool> makeMaiZlibDecompressTool();
