#include <cstdio>
#include <string>

#include <json.hpp>

#include "MaiKlingMediaTools.h"
#include "MaiMiniMaxMediaTools.h"
#include "MaiFilePath.h"
#include "MaiFileSystem.h"
#include "MaiIdGenerator.h"
#include "MaiMemoryStore.h"
#include "MaiTime.h"

namespace {

int failures = 0;

#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
            ++failures;                                                     \
        }                                                                   \
    } while (0)

void checkTool(MaiTool& tool, const std::string& expectedName, const std::string& expectedModel,
               bool video) {
    MaiToolContext context;
    context.root = MaiFileSystem::temporaryDirectory().toUtf8();
    CHECK(tool.name() == expectedName);
    CHECK(nlohmann::json::parse(tool.parametersSchema()).is_object());
    CHECK(!tool.requiresPerCallApproval(R"({"action":"discover"})"));
    CHECK(!tool.requiresPerCallApproval(R"({"action":"continue"})"));
    CHECK(tool.requiresPerCallApproval(R"({"action":"delegate"})") ==
          (expectedName != "minimax_video"));
    CHECK(tool.requiresPerCallApproval("broken json"));
    const auto discovered =
        nlohmann::json::parse(tool.execute(R"({"action":"discover"})", context).output());
    CHECK(discovered.at("bound_model") == expectedModel);
    CHECK(discovered.at("configured") == false);
    CHECK(discovered.at("capabilities")[0].at("tool_status") == "not_configured");
    CHECK(discovered.dump().find("secret") == std::string::npos);
    if (expectedName == "minimax_video") {
        CHECK(discovered.dump().find("Hailuo-2.3") == std::string::npos);
        CHECK(tool.parametersSchema().find("MiniMax-Hailuo-2.3") == std::string::npos);
    }
    CHECK(tool.execute(R"({"action":"delegate","message":"A running cat"})", context).hasError());
    CHECK(tool.execute(R"({"action":"continue","conversation_id":"missing"})", context).hasError());
    if (video && expectedName == "kling_video")
        CHECK(discovered.dump().find("existing_video_edit") != std::string::npos);
    if (video && expectedName == "minimax_video")
        CHECK(discovered.dump().find("multi_reference_video") != std::string::npos);
}

void testFailedTaskIsStableWithoutProviderRequest() {
    auto store = makeMaiMemoryStore();
    MaiSession session;
    session.id = "session";
    session.directory = "/tmp";
    session.model = "test-model";
    store->putSession(session);
    MaiSpecialistTask task;
    task.id = MaiIdGenerator::generate("spt_");
    task.ownerSessionId = "session";
    task.specialistName = "minimax_video";
    task.providerTaskId = "h3_12345";
    task.intent = "A child waves";
    task.inputReference = "first.jpg";
    task.created = MaiTime::getCurrentTime();
    CHECK(!store->insertSpecialistTask(task));
    CHECK(
        !store->finishSpecialistTask(task.id, task.ownerSessionId, MaiSpecialistTaskStatus::Failed,
                                     "input text sensitive (1026)", {}, MaiTime::getCurrentTime()));
    MaiToolContext context;
    context.sessionId = task.ownerSessionId;
    context.specialistTasks = store.get();
    auto video = makeMaiMiniMaxVideoTool([] { return std::string("test-key"); });
    const MaiToolResult result = video->execute(
        nlohmann::json{{"action", "continue"}, {"conversation_id", task.id}}.dump(), context);
    CHECK(!result.hasError());
    const auto reply = nlohmann::json::parse(result.output());
    CHECK(reply.at("status") == "failed");
    CHECK(reply.at("input_reference_present") == true);
    CHECK(reply.at("reply") == "input text sensitive (1026)");
}

std::string localErrorMessage(MaiTool& tool, const nlohmann::json& args,
                              const MaiToolContext& context) {
    const MaiToolResult result = tool.execute(args.dump(), context);
    CHECK(result.hasError());
    if (!result.hasError()) return {};
    const auto error = nlohmann::json::parse(result.error().message(), nullptr, false);
    return error.is_object() && error.value("message", nlohmann::json{}).is_string()
               ? error["message"].get<std::string>()
               : std::string{};
}

void testH3ContentRolesAndRequiredFields() {
    using Json = nlohmann::json;
    auto video = makeMaiMiniMaxVideoTool([] { return std::string("test-key"); });
    MaiToolContext context;
    context.root = MaiFileSystem::temporaryDirectory().toUtf8();
    Json base = {{"action", "delegate"},
                 {"model", "MiniMax-H3"},
                 {"duration", 10},
                 {"resolution", "768P"},
                 {"message", "A child walks in the rain"}};
    for (const std::string& role : {"reference", "", "reference_image"}) {
        Json args = base;
        args["content"] = Json::array();
        for (int index = 0; index < 3; ++index) {
            Json image = {{"type", "image_url"},
                          {"path", "missing-reference-" + std::to_string(index) + ".jpg"}};
            if (!role.empty()) image["role"] = role;
            args["content"].push_back(std::move(image));
        }
        const std::string error = localErrorMessage(*video, args, context);
        CHECK(error.find("Reference file") == 0);
    }

    Json mixed = base;
    mixed["content"] = Json::array(
        {Json{{"type", "text"}, {"text", "Keep the same child"}},
         Json{{"type", "image_url"}, {"role", "reference"}, {"path", "missing-reference.jpg"}},
         Json{{"type", "video_url"}, {"role", "reference"}, {"path", "missing-reference.mp4"}},
         Json{{"type", "audio_url"}, {"path", "missing-reference.wav"}}});
    CHECK(localErrorMessage(*video, mixed, context).find("Reference file") == 0);

    Json nine = base;
    nine["content"] = Json::array();
    for (int index = 0; index < 9; ++index)
        nine["content"].push_back(
            Json{{"type", "image_url"},
                 {"path", "missing-reference-" + std::to_string(index) + ".jpg"}});
    CHECK(localErrorMessage(*video, nine, context).find("Reference file") == 0);
    nine["content"].push_back(Json{{"type", "image_url"}, {"path", "missing-reference-10.jpg"}});
    CHECK(localErrorMessage(*video, nine, context) == "Reference count exceeds the H3 input limit");

    Json textRole = base;
    textRole["content"] = Json::array(
        {Json{{"type", "text"}, {"role", "first_frame"}, {"text", "A rainy night"}},
         Json{{"type", "image_url"}, {"role", "reference"}, {"path", "missing-first.jpg"}},
         Json{{"type", "image_url"}, {"role", "reference"}, {"path", "missing-second.jpg"}},
         Json{{"type", "image_url"}, {"role", "reference"}, {"path", "missing-third.jpg"}}});
    CHECK(localErrorMessage(*video, textRole, context).find("Reference file") == 0);

    Json noPrompt = base;
    noPrompt.erase("message");
    noPrompt["content"] = Json::array({Json{{"type", "image_url"}, {"path", "missing-first.jpg"}},
                                       Json{{"type", "image_url"}, {"path", "missing-second.jpg"}},
                                       Json{{"type", "image_url"}, {"path", "missing-third.jpg"}}});
    CHECK(localErrorMessage(*video, noPrompt, context) ==
          "A text instruction is required and must be at most 8000 bytes");

    Json missingResolution = base;
    missingResolution.erase("resolution");
    missingResolution["content"] =
        Json::array({Json{{"type", "image_url"}, {"role", "reference"}, {"path", "missing.jpg"}}});
    CHECK(localErrorMessage(*video, missingResolution, context) ==
          "Missing top-level resolution for paid video generation");
}

void testH3RejectsUnsupportedOutputBeforePaidApproval() {
    auto video = makeMaiMiniMaxVideoTool([] { return std::string("test-key"); });
    MaiToolContext context;
    context.root = MaiFileSystem::temporaryDirectory().toUtf8();
    for (const std::string& resolution : {"1080P", "720P"}) {
        const nlohmann::json args = {{"action", "delegate"},
                                     {"model", "MiniMax-H3"},
                                     {"message", "A running cat"},
                                     {"duration", 10},
                                     {"resolution", resolution}};
        CHECK(!video->requiresPerCallApproval(args.dump()));
        CHECK(localErrorMessage(*video, args, context).find("native 1080P and 720P") !=
              std::string::npos);
    }
    for (const std::string& resolution : {"768P", "2K"}) {
        const nlohmann::json args = {{"action", "delegate"},
                                     {"model", "MiniMax-H3"},
                                     {"duration", 10},
                                     {"resolution", resolution}};
        CHECK(video->requiresPerCallApproval(args.dump()));
    }
}

void testH3LocalValidationChecksAllReferencePathsWithoutBilling() {
    using Json = nlohmann::json;
    const MaiFilePath root = MaiFileSystem::temporaryDirectory().append(
        MaiFilePath::fromUtf8(MaiIdGenerator::generate("mai-h3-validation-")));
    CHECK(!MaiFileSystem::createDirectories(root));
    const std::string first = "first.jpg";
    const std::string second = "second.jpeg";
    const std::string third = "third.png";
    for (const std::string& name : {first, second, third})
        CHECK(!MaiFileSystem::writeFile(root.append(MaiFilePath::fromUtf8(name)),
                                        std::string("image bytes")));
    int keyReads = 0;
    auto video = makeMaiMiniMaxVideoTool([&] {
        ++keyReads;
        return std::string{};
    });
    MaiToolContext context;
    context.root = root.toUtf8();
    const Json args = {
        {"action", "validate"},
        {"model", "MiniMax-H3"},
        {"duration", 10},
        {"resolution", "768P"},
        {"content",
         Json::array({Json{{"type", "text"},
                           {"role", "first_frame"},
                           {"text", "The same child walks in the rain"}},
                      Json{{"type", "image_url"}, {"role", "reference"}, {"path", first}},
                      Json{{"type", "image_url"}, {"path", second}},
                      Json{{"type", "image_url"}, {"role", "reference_image"}, {"path", third}}})}};
    CHECK(!video->requiresPerCallApproval(args.dump()));
    const MaiToolResult result = video->execute(args.dump(), context);
    CHECK(!result.hasError());
    if (!result.hasError()) {
        const Json preview = Json::parse(result.output());
        CHECK(preview.at("status") == "validated");
        CHECK(preview.at("submitted") == false);
        CHECK(preview.at("uploaded") == false);
        CHECK(preview.at("charged") == false);
        const Json& body = preview.at("request_preview");
        CHECK(body.at("duration") == 10);
        CHECK(body.at("resolution") == "768P");
        CHECK(body.at("content").size() == 4);
        for (std::size_t index = 1; index < body.at("content").size(); ++index) {
            CHECK(body.at("content")[index].at("role") == "reference_image");
            CHECK(body.at("content")[index].at("file_size_bytes") == 11);
        }
    }
    CHECK(keyReads == 0);
    CHECK(!MaiFileSystem::removeFile(root.append(MaiFilePath::fromUtf8(second))));
    CHECK(localErrorMessage(*video, args, context) ==
          "Reference file exceeds its provider size limit");
    CHECK(keyReads == 0);
    MaiFileSystem::removeRecursively(root);
}

}  // namespace

int main() {
    testFailedTaskIsStableWithoutProviderRequest();
    testH3ContentRolesAndRequiredFields();
    testH3RejectsUnsupportedOutputBeforePaidApproval();
    testH3LocalValidationChecksAllReferencePathsWithoutBilling();
    std::string key;
    auto klingVideo = makeMaiKlingVideoTool([&] { return key; });
    auto klingImage = makeMaiKlingImageTool([&] { return key; });
    auto miniMaxVideo = makeMaiMiniMaxVideoTool([&] { return key; });
    auto miniMaxImage = makeMaiMiniMaxImageTool([&] { return key; });
    checkTool(*klingVideo, "kling_video", "kling-3.0-turbo", true);
    checkTool(*klingImage, "kling_image", "kling-v3-omni", false);
    checkTool(*miniMaxVideo, "minimax_video", "MiniMax-H3", true);
    checkTool(*miniMaxImage, "minimax_image", "image-01", false);
    key = "secret-test-key";
    MaiToolContext context;
    context.root = MaiFileSystem::temporaryDirectory().toUtf8();
    for (MaiTool* tool :
         {klingVideo.get(), klingImage.get(), miniMaxVideo.get(), miniMaxImage.get()}) {
        const auto discovered =
            nlohmann::json::parse(tool->execute(R"({"action":"discover"})", context).output());
        CHECK(discovered.at("configured") == true);
        CHECK(discovered.at("capabilities")[0].at("tool_status") == "implemented_unverified");
        CHECK(discovered.dump().find(key) == std::string::npos);
        CHECK(tool->execute(R"({"action":"delegate","message":"A cat","video_path":"old.mp4"})",
                            context)
                  .hasError());
    }
    CHECK(
        klingVideo
            ->execute(R"({"action":"delegate","message":"A cat","duration":7,"resolution":"720p"})",
                      context)
            .hasError());
    CHECK(
        klingVideo
            ->execute(
                R"({"action":"delegate","message":"A cat","duration":5,"resolution":"720p","last_frame_path":"last.jpg"})",
                context)
            .hasError());
    CHECK(miniMaxVideo
              ->execute(
                  R"({"action":"delegate","message":"A cat","duration":10,"resolution":"1080P"})",
                  context)
              .hasError());
    CHECK(
        miniMaxVideo
            ->execute(
                R"({"action":"delegate","model":"MiniMax-H3","message":"A cat","duration":5,"resolution":"1080P","ratio":"9:16"})",
                context)
            .hasError());
    CHECK(
        miniMaxVideo
            ->execute(
                R"({"action":"delegate","model":"MiniMax-H3","message":"A cat","duration":10,"resolution":"768P","ratio":"9:16","content":[{"type":"image_url","role":"first_frame","path":"first.jpg"},{"type":"image_url","role":"reference_image","path":"reference.jpg"}]})",
                context)
            .hasError());
    CHECK(
        miniMaxVideo
            ->execute(
                R"({"action":"delegate","model":"MiniMax-H3","message":"A cat","duration":10,"resolution":"768P","ratio":"9:16","content":[{"type":"image_url","role":"reference_image","path":"missing.jpg"}]})",
                context)
            .hasError());
    CHECK(klingImage->execute(R"({"action":"delegate","message":"A cat","ratio":"3:4"})", context)
              .hasError());
    return failures == 0 ? 0 : 1;
}
