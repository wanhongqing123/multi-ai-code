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
    CHECK(request[2].content == "Analyze this video");
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

}  // namespace

int main() {
    testCurrentRequestKeepsOnlyRecentToolPixels();
    testSmallCurrentRequestKeepsEveryImage();
    return failures == 0 ? 0 : 1;
}
