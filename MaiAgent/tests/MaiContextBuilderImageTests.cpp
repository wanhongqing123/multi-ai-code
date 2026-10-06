#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "MaiContextBuilder.h"
#include "MaiMessage.h"

static int failures = 0;
#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
            ++failures;                                                     \
        }                                                                   \
    } while (0)

namespace {

MaiMessage userMessage(const std::string& text, const std::string& imagePath) {
    MaiMessage message;
    message.role = MaiRole::User;
    if (!text.empty()) {
        MaiMessagePart part;
        part.body = MaiTextPart{text};
        message.parts.push_back(std::move(part));
    }
    if (!imagePath.empty()) {
        MaiMessagePart part;
        part.body = MaiImagePart{imagePath, "image/jpeg"};
        message.parts.push_back(std::move(part));
    }
    return message;
}

MaiMessage toolImages(const std::string& prefix, int count) {
    MaiMessage message;
    message.role = MaiRole::Assistant;
    for (int index = 0; index < count; ++index) {
        MaiMessagePart part;
        part.body = MaiImagePart{prefix + std::to_string(index) + ".jpg", "image/jpeg"};
        message.parts.push_back(std::move(part));
    }
    return message;
}

void testCurrentRequestKeepsOnlyRecentToolPixels() {
    const std::vector<MaiMessage> history = {
        userMessage("Earlier request", "old-user.jpg"), toolImages("old-tool-", 3),
        userMessage("Analyze this video", "current-user.jpg"), toolImages("frame-", 39)};
    const std::vector<MaiModelMessage> request = MaiContextBuilder().build(history);
    std::vector<std::string> images;
    for (const auto& message : request) {
        for (const auto& image : message.images) images.push_back(image.path);
    }
    CHECK(images.size() == 9);
    CHECK(images.front() == "current-user.jpg");
    for (int index = 0; index < 8; ++index)
        CHECK(images[index + 1] == "frame-" + std::to_string(index + 31) + ".jpg");
    CHECK(request.front().role == MaiModelRole::System);
    CHECK(request.front().content.find("Omitted 35 older image observations") != std::string::npos);
    CHECK(request.front().content.find("view_image") != std::string::npos);
    CHECK(request[2].content.find("Analyze this video") != std::string::npos);
    CHECK(request[2].content.find("current-user.jpg") != std::string::npos);
    CHECK(request.back().images.front().path == "frame-38.jpg");
}

void testSmallCurrentRequestKeepsEveryImage() {
    const std::vector<MaiMessage> history = {userMessage("Inspect these", {}),
                                             toolImages("frame-", 4)};
    const std::vector<MaiModelMessage> request = MaiContextBuilder().build(history);
    int imageCount = 0;
    for (const auto& message : request) imageCount += static_cast<int>(message.images.size());
    CHECK(imageCount == 4);
    CHECK(request.back().images.front().path == "frame-3.jpg");
}

void testVideoAttachmentUsesAFileReferenceInsteadOfImagePixels() {
    MaiMessage message = userMessage("Please edit this video", {});
    MaiMessagePart video;
    video.body = MaiVideoPart{"clips/input.mp4", "video/mp4"};
    message.parts.push_back(std::move(video));
    const std::vector<MaiModelMessage> request = MaiContextBuilder().build({message});
    CHECK(request.size() == 1);
    CHECK(request.front().images.empty());
    CHECK(request.front().content.find("clips/input.mp4") != std::string::npos);
}

void testCurrentImagesHaveExactPathAndAttachmentIdentity() {
    MaiMessage message = userMessage("Use these two grass photos", {});
    MaiMessagePart first;
    first.id = "prt_photo_one";
    first.body = MaiImagePart{"gallery/child grass 1.jpg", "image/jpeg"};
    message.parts.push_back(first);
    MaiMessagePart second;
    second.id = "prt_photo_two";
    second.body = MaiImagePart{"gallery/child grass 2.jpg", "image/jpeg"};
    message.parts.push_back(second);
    const auto request = MaiContextBuilder().build({message});
    CHECK(request.size() == 1);
    CHECK(request[0].images.size() == 2);
    CHECK(request[0].content.find("prt_photo_one") != std::string::npos);
    CHECK(request[0].content.find("gallery/child grass 1.jpg") != std::string::npos);
    CHECK(request[0].content.find("prt_photo_two") != std::string::npos);
    CHECK(request[0].content.find("gallery/child grass 2.jpg") != std::string::npos);
}

void testRecentPreviousImagePathsRemainAvailableWithoutOldPixels() {
    MaiContextBuilder::Options options;
    options.maxRecentUserImageReferences = 2;
    const std::vector<MaiMessage> history = {
        userMessage("first", "oldest.jpg"), userMessage("second", "recent-one.jpg"),
        userMessage("third", "recent-two.jpg"), userMessage("Use the last two", {})};
    const auto request = MaiContextBuilder(options).build(history);
    for (const auto& message : request) CHECK(message.images.empty());
    bool sawOldest = false;
    bool sawFirstRecent = false;
    bool sawSecondRecent = false;
    for (const auto& message : request) {
        sawOldest |= message.content.find("oldest.jpg") != std::string::npos;
        sawFirstRecent |= message.content.find("recent-one.jpg") != std::string::npos;
        sawSecondRecent |= message.content.find("recent-two.jpg") != std::string::npos;
    }
    CHECK(!sawOldest);
    CHECK(sawFirstRecent);
    CHECK(sawSecondRecent);
}

void testQuotedUserMediaUsesExactSourceMessage() {
    MaiMessage source = userMessage("Earlier photo", "gallery/exact-photo.jpg");
    source.id = "msg_photo";
    source.parts.back().id = "prt_photo";
    MaiMessage quoted = userMessage("Save the photo I quoted", {});
    quoted.id = "msg_followup";
    MaiMessagePart quote;
    quote.body = MaiQuotePart{source.id, "Earlier photo"};
    quoted.parts.insert(quoted.parts.begin(), quote);
    const auto request = MaiContextBuilder().build({source, quoted});
    CHECK(request.back().role == MaiModelRole::User);
    CHECK(request.back().content.find("msg_photo") != std::string::npos);
    CHECK(request.back().content.find("prt_photo") != std::string::npos);
    CHECK(request.back().content.find("gallery/exact-photo.jpg") != std::string::npos);
    CHECK(request.back().images.empty());
}

void testQuotedAssistantMediaUsesDeliveredArtifact() {
    MaiMessage source;
    source.id = "msg_generated";
    source.role = MaiRole::Assistant;
    MaiMessagePart artifact;
    artifact.id = "prt_delivered_video";
    MaiToolPart tool;
    tool.tool = "agent_send_media";
    tool.state = MaiToolState::Completed;
    tool.output =
        R"({"delivery":"current_ai_session","path":"generated/final.mp4","type":"video"})";
    artifact.body = tool;
    source.parts.push_back(artifact);
    MaiMessage quoted = userMessage("Save that video", {});
    quoted.id = "msg_followup";
    MaiMessagePart quote;
    quote.body = MaiQuotePart{source.id, "Media message"};
    quoted.parts.insert(quoted.parts.begin(), quote);
    const auto request = MaiContextBuilder().build({source, quoted});
    CHECK(request.back().content.find("msg_generated") != std::string::npos);
    CHECK(request.back().content.find("prt_delivered_video") != std::string::npos);
    CHECK(request.back().content.find("generated/final.mp4") != std::string::npos);
}

}  // namespace

int main() {
    testCurrentRequestKeepsOnlyRecentToolPixels();
    testSmallCurrentRequestKeepsEveryImage();
    testVideoAttachmentUsesAFileReferenceInsteadOfImagePixels();
    testCurrentImagesHaveExactPathAndAttachmentIdentity();
    testRecentPreviousImagePathsRemainAvailableWithoutOldPixels();
    testQuotedUserMediaUsesExactSourceMessage();
    testQuotedAssistantMediaUsesDeliveredArtifact();
    return failures == 0 ? 0 : 1;
}
