#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

#include <json.hpp>

#include "MaiGlmMediaTools.h"

namespace {

using Json = nlohmann::json;

std::string value(const Json& data, const char* field) {
    return data.is_object() && data.contains(field) && data[field].is_string()
               ? data[field].get<std::string>()
               : std::string{};
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: MaiGlmLiveParity <workspace-image-path>\n";
        return 2;
    }
    const char* key = std::getenv("MAIAGENT_GLM_LIVE_KEY");
    if (key == nullptr || *key == '\0') {
        std::cerr << "MAIAGENT_GLM_LIVE_KEY is required\n";
        return 2;
    }
    auto tool = makeMaiGlmVideoTool([credential = std::string(key)] { return credential; });
    MaiToolContext context;
    context.root = "/tmp";
    const Json request = {
        {"action", "delegate"},
        {"message",
         "The child in the reference photo smiles and waves gently with one hand, "
         "staying in the same room; preserve face and clothes, no dialogue."},
        {"image_path", argv[1]},
        {"duration", 5},
        {"size", "1920x1080"},
        {"fps", 30},
        {"quality", "quality"},
        {"with_audio", true},
    };
    const MaiToolResult submitted = tool->execute(request.dump(), context);
    if (submitted.hasError()) {
        std::cerr << "submit_error " << submitted.error().message() << '\n';
        return 1;
    }
    const Json task = Json::parse(submitted.output(), nullptr, false);
    const std::string id = value(task, "conversation_id");
    if (id.empty()) {
        std::cerr << "submit returned no task ID\n";
        return 1;
    }
    std::cout << "submitted " << id << std::endl;
    for (int attempt = 0; attempt < 60; ++attempt) {
        std::this_thread::sleep_for(std::chrono::seconds(8));
        const MaiToolResult progress =
            tool->execute(Json{{"action", "continue"}, {"conversation_id", id}}.dump(), context);
        if (progress.hasError()) {
            std::cerr << "query_error " << progress.error().message() << '\n';
            return 1;
        }
        const Json response = Json::parse(progress.output(), nullptr, false);
        const std::string status = value(response, "status");
        if (status == "succeeded") {
            std::cout << "succeeded " << value(response, "path") << std::endl;
            return 0;
        }
        if (status == "failed") {
            std::cerr << "failed " << value(response, "reply") << '\n';
            return 1;
        }
        if (attempt == 0 || attempt % 6 == 5) std::cout << "status " << status << std::endl;
    }
    std::cerr << "timed out waiting for GLM task\n";
    return 1;
}
