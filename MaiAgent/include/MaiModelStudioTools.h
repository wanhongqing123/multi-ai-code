#pragma once

#include <functional>
#include <memory>
#include <string>

#include "MaiTool.h"

// Credentials are read at call time so a host can update or revoke them without recreating the
// Agent. The workspace ID is the Model Studio workspace, not a local file path. Both values must
// belong to the same Beijing region. This callback runs on a tool worker, never on a UI thread.
struct MaiWanCredentials {
    std::string apiKey;
    std::string workspaceId;
};
using MaiWanCredentialsProvider = std::function<MaiWanCredentials()>;

// Edits a 2-10 second MP4/MOV with wan2.7-videoedit. Local input is uploaded to Alibaba's private
// temporary storage; the output is downloaded into the Agent workspace. The source is unchanged.
// execute() can block for upload/download and checks MaiToolContext cancellation. An empty
// credential leaves discovery available but refuses paid submission.
std::unique_ptr<MaiTool> makeMaiWanVideoEditTool(MaiWanCredentialsProvider credentials,
                                                 std::string caBundlePath = {});

// Wan3.0 generates a new video from text, first/last frames, or reference media. Local video
// references use the same private temporary upload route as Wan2.7. Task submission is async;
// the completed MP4 is downloaded to the Agent workspace by continue().
std::unique_ptr<MaiTool> makeMaiWanVideoTool(MaiWanCredentialsProvider credentials,
                                             std::string caBundlePath = {});

// Qwen-Image-3.0-Pro creates or edits a PNG from text and up to three local images. It sends
// bounded Base64 image content to Model Studio, saves a new PNG under the Agent workspace, and
// never modifies the source images. Calls can block while the remote model generates an image.
std::unique_ptr<MaiTool> makeMaiQwenImageTool(MaiWanCredentialsProvider credentials,
                                              std::string caBundlePath = {});
