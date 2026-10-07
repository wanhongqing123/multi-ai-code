#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

#include <json.hpp>

#include "MaiMiniMaxMediaTools.h"

namespace {

using Json = nlohmann::json;

std::string value(const Json& data, const char* field) {
    return data.is_object() && data.contains(field) && data[field].is_string()
               ? data[field].get<std::string>()
               : std::string{};
}

}  // namespace

int main(int argc, char** argv) {
    const bool references = argc == 5 && std::string(argv[1]) == "--references";
    const bool validateReferences = argc == 5 && std::string(argv[1]) == "--validate-references";
    const bool high = argc == 3 && std::string(argv[1]) == "--high";
    if (argc != 2 && !references && !validateReferences && !high) {
        std::cerr << "usage: MaiMiniMaxLiveParity <first-frame-path> | "
                     "--high <first-frame-path> | --validate-references <image-one> "
                     "<image-two> <image-three> | --references <image-one> <image-two> "
                     "<image-three>\n";
        return 2;
    }
    const char* key = std::getenv("MAIAGENT_MINIMAX_LIVE_KEY");
    if (!validateReferences && (key == nullptr || *key == '\0')) {
        std::cerr << "MAIAGENT_MINIMAX_LIVE_KEY is required\n";
        return 2;
    }
    auto tool = makeMaiMiniMaxVideoTool(
        [credential = key == nullptr ? std::string{} : std::string(key)] { return credential; });
    MaiToolContext context;
    context.root = "/tmp";
    Json request = {
        {"action", "delegate"},
        {"model", "MiniMax-H3"},
        {"message",
         "The child in the first frame gently lifts one hand and waves at the "
         "camera, then smiles naturally. Keep the child's face and clothing "
         "consistent, stay in the same room, no dialogue."},
        {"image_path", high ? argv[2] : argv[1]},
        {"duration", 4},
        {"resolution", "768P"},
        {"ratio", "adaptive"},
    };
    if (high) {
        request["duration"] = 10;
        request["resolution"] = "2K";
    }
    if (references || validateReferences) {
        request.erase("image_path");
        request["message"] =
            "Use all three photos as references for the same child. The child "
            "stands safely under a covered walkway during gentle evening rain, "
            "smiles and waves. Preserve facial proportions and the white outfit, "
            "with natural movement and no dialogue.";
        request["content"] = Json::array(
            {Json{{"type", "image_url"}, {"role", "reference"}, {"path", argv[2]}},
             Json{{"type", "image_url"}, {"path", argv[3]}},
             Json{{"type", "image_url"}, {"role", "reference_image"}, {"path", argv[4]}}});
        request["ratio"] = "9:16";
    }
    if (validateReferences) request["action"] = "validate";
    const MaiToolResult submitted = tool->execute(request.dump(), context);
    if (submitted.hasError()) {
        std::cerr << "submit_error " << submitted.error().message() << '\n';
        return 1;
    }
    Json reply = Json::parse(submitted.output(), nullptr, false);
    if (validateReferences) {
        std::cout << reply.dump(2) << std::endl;
        return value(reply, "status") == "validated" ? 0 : 1;
    }
    if (value(reply, "status") == "succeeded") {
        std::cout << "succeeded " << value(reply, "path") << std::endl;
        return 0;
    }
    const std::string id = value(reply, "conversation_id");
    if (id.empty()) {
        std::cerr << "submit returned no task ID\n";
        return 1;
    }
    std::cout << "submitted " << value(reply, "task_id") << std::endl;
    for (int attempt = 0; attempt < 60; ++attempt) {
        std::this_thread::sleep_for(std::chrono::seconds(8));
        const MaiToolResult progress =
            tool->execute(Json{{"action", "continue"}, {"conversation_id", id}}.dump(), context);
        if (progress.hasError()) {
            std::cerr << "query_error " << progress.error().message() << '\n';
            return 1;
        }
        reply = Json::parse(progress.output(), nullptr, false);
        const std::string status = value(reply, "status");
        if (status == "succeeded") {
            std::cout << "succeeded " << value(reply, "path") << std::endl;
            return 0;
        }
        if (status == "failed") {
            std::cerr << "failed " << value(reply, "reply") << '\n';
            return 1;
        }
        if (attempt == 0 || attempt % 6 == 5) std::cout << "status " << status << std::endl;
    }
    std::cerr << "timed out waiting for MiniMax task\n";
    return 1;
}
