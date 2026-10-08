#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

#include "MaiAgent.h"
#include "MaiFakeModelClient.h"
#include "MaiIdGenerator.h"
#include "MaiMemoryStore.h"
#include "MaiTime.h"
#include "MaiTool.h"

namespace {

int failures = 0;

#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
            ++failures;                                                     \
        }                                                                   \
    } while (0)

class StaticMediaTool final : public MaiTool {
public:
    StaticMediaTool(std::string result, bool success, std::string toolName)
        : mResult(std::move(result)), mSuccess(success), mToolName(std::move(toolName)) {}

    std::string name() const override {
        return mToolName;
    }
    std::string description() const override {
        return "Fake video task for notification tests";
    }
    std::string parametersSchema() const override {
        return R"({"type":"object"})";
    }
    MaiToolResult execute(const std::string&, const MaiToolContext&) override {
        return mSuccess ? MaiToolResult::success(mResult)
                        : MaiToolResult::failure(MaiErrorCode::Network, mResult);
    }

private:
    std::string mResult;
    bool mSuccess;
    std::string mToolName;
};

struct Scenario {
    std::unique_ptr<MaiAgent> agent;
    MaiSpecialistTaskStore* store = nullptr;
    MaiFakeModelClient* fakeModel = nullptr;
    std::string sessionId;
    std::string taskId;
};

Scenario startScenario(std::string result, bool withModel, bool success = false,
                       bool modelSucceeds = false, std::string specialistName = "wan_video") {
    auto store = makeMaiMemoryStore();
    Scenario scenario;
    scenario.store = store.get();
    MaiSession session;
    session.id = MaiIdGenerator::newSessionId();
    session.directory = "/tmp";
    session.model = "test-model";
    store->putSession(session);
    scenario.sessionId = session.id;

    MaiSpecialistTask task;
    task.id = MaiIdGenerator::generate("spt_");
    task.ownerSessionId = session.id;
    task.specialistName = specialistName;
    task.providerTaskId = "provider-task";
    task.intent = "Create a video";
    task.created = MaiTime::getCurrentTime();
    CHECK(!store->insertSpecialistTask(task));
    scenario.taskId = task.id;

    std::unique_ptr<MaiModelClient> model;
    if (withModel) {
        auto fake = std::make_unique<MaiFakeModelClient>();
        scenario.fakeModel = fake.get();
        MaiFakeModelClient::Turn reply;
        if (modelSucceeds)
            reply.textChunks = {"I will inspect the failure."};
        else
            reply.error = MaiError::make(MaiErrorCode::Network, "Main model is offline");
        fake->setRepeatingTurn(reply);
        model = std::move(fake);
    }
    auto tools = std::make_unique<MaiToolRegistry>();
    tools->add(
        std::make_unique<StaticMediaTool>(std::move(result), success, std::move(specialistName)));
    scenario.agent =
        std::make_unique<MaiAgent>(std::move(store), std::move(model), std::move(tools));
    return scenario;
}

void waitForStatus(Scenario& scenario, MaiSpecialistTaskStatus status) {
    for (int attempt = 0; attempt < 100; ++attempt) {
        MaiSpecialistTask task;
        if (scenario.store->getSpecialistTask(scenario.taskId, scenario.sessionId, task) &&
            task.status == status &&
            (status != MaiSpecialistTaskStatus::Running || task.lastCheckedAt != 0))
            return;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(false);
}

void testFailureVisibleEvenWhenMainModelFails() {
    auto scenario =
        startScenario(R"({"code":"task_failed","message":"Provider rejected this video"})", true);
    waitForStatus(scenario, MaiSpecialistTaskStatus::Failed);
    bool visible = false;
    for (int attempt = 0; attempt < 100; ++attempt) {
        for (const MaiMessage& message : scenario.agent->listMessages(scenario.sessionId)) {
            if (message.text().find("Video task failed. Provider rejected this video") == 0)
                visible = true;
        }
        if (visible) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(visible);
}

void testAuthorizationFailureIsTerminalWithoutMainModel() {
    auto scenario = startScenario(
        R"({"code":"provider_error","http_status":403,"message":"Access denied"})", false);
    waitForStatus(scenario, MaiSpecialistTaskStatus::Failed);
    bool visible = false;
    for (int attempt = 0; attempt < 100; ++attempt) {
        for (const MaiMessage& message : scenario.agent->listMessages(scenario.sessionId)) {
            if (message.text().find("Video task status unavailable. Access denied") == 0)
                visible = true;
        }
        if (visible) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(visible);
}

void testRateLimitRemainsRetryable() {
    auto scenario = startScenario(
        R"({"code":"provider_error","http_status":429,"message":"Rate limited"})", true);
    waitForStatus(scenario, MaiSpecialistTaskStatus::Running);
    MaiSpecialistTask task;
    CHECK(scenario.store->getSpecialistTask(scenario.taskId, scenario.sessionId, task));
    CHECK(task.status == MaiSpecialistTaskStatus::Running);
    CHECK(scenario.agent->listMessages(scenario.sessionId).empty());
}

void testBalanceErrorHandsTaskBackToMainWithoutExposingFailure() {
    auto scenario = startScenario(
        R"({"code":"provider_error","provider_code":1102,"http_status":429,"message":"Account balance not enough"})",
        true, false, true);
    waitForStatus(scenario, MaiSpecialistTaskStatus::Failed);
    MaiSpecialistTask task;
    CHECK(scenario.store->getSpecialistTask(scenario.taskId, scenario.sessionId, task));
    CHECK(task.errorText.find("submitted task's outcome has not been verified") !=
          std::string::npos);
    for (int attempt = 0; attempt < 100 && scenario.fakeModel->requestCount() == 0; ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(scenario.fakeModel->requestCount() > 0);
    if (scenario.fakeModel->requestCount() == 0) return;
    bool sawRecovery = false;
    for (const MaiModelMessage& message : scenario.fakeModel->lastRequest().messages) {
        if (message.role == MaiModelRole::System &&
            message.content.find("Check other configured specialist tools") != std::string::npos)
            sawRecovery = true;
    }
    CHECK(sawRecovery);
    for (const MaiMessage& message : scenario.agent->listMessages(scenario.sessionId)) {
        CHECK(message.text().find("Video task status unavailable.") == std::string::npos);
        CHECK(message.text().find("Video task failed.") == std::string::npos);
    }
}

void testRepeatedRateLimitEventuallyNotifiesMainConversation() {
    auto scenario = startScenario(
        R"({"code":"provider_error","http_status":429,"message":"Rate limited"})", false);
    bool terminal = false;
    for (int attempt = 0; attempt < 2500; ++attempt) {
        MaiSpecialistTask task;
        if (scenario.store->getSpecialistTask(scenario.taskId, scenario.sessionId, task) &&
            task.status == MaiSpecialistTaskStatus::Failed) {
            CHECK(task.errorText.find("Repeatedly failed after 3 attempts") != std::string::npos);
            terminal = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(terminal);
    bool visible = false;
    for (int attempt = 0; attempt < 100; ++attempt) {
        for (const MaiMessage& message : scenario.agent->listMessages(scenario.sessionId)) {
            if (message.text().find("Video task status unavailable. Repeatedly failed") == 0)
                visible = true;
        }
        if (visible) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(visible);
}

void testCompletedWithoutOutputShowsFailure() {
    auto scenario = startScenario(R"({"status":"succeeded","reply":"Done"})", false, true);
    waitForStatus(scenario, MaiSpecialistTaskStatus::Failed);
    bool visible = false;
    for (int attempt = 0; attempt < 100; ++attempt) {
        for (const MaiMessage& message : scenario.agent->listMessages(scenario.sessionId)) {
            if (message.text().find("Video output unavailable. The provider completed the task") ==
                0)
                visible = true;
        }
        if (visible) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(visible);
}

void testProviderFailedStatusNotifiesMainConversation() {
    auto scenario = startScenario(
        R"json({"status":"failed","reply":"input text sensitive (1026)"})json", false, true);
    waitForStatus(scenario, MaiSpecialistTaskStatus::Failed);
    MaiSpecialistTask task;
    CHECK(scenario.store->getSpecialistTask(scenario.taskId, scenario.sessionId, task));
    CHECK(task.finalText == "input text sensitive (1026)");
    bool visible = false;
    for (int attempt = 0; attempt < 100; ++attempt) {
        for (const MaiMessage& message : scenario.agent->listMessages(scenario.sessionId)) {
            if (message.text().find("Video task failed. input text sensitive (1026)") == 0)
                visible = true;
        }
        if (visible) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(visible);
}

void testFailedSpecialistAddsRecoveryInstructionToMainModel() {
    auto scenario = startScenario(
        R"({"status":"failed","reply":"Provider rejected the reference image"})", true, true, true);
    waitForStatus(scenario, MaiSpecialistTaskStatus::Failed);
    for (int attempt = 0; attempt < 100 && scenario.fakeModel->requestCount() == 0; ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(scenario.fakeModel->requestCount() > 0);
    if (scenario.fakeModel->requestCount() == 0) return;
    const MaiModelRequest request = scenario.fakeModel->lastRequest();
    bool sawRecovery = false;
    for (const MaiModelMessage& message : request.messages) {
        if (message.role == MaiModelRole::System &&
            message.content.find(
                "This attempt failed. The provider identified the reference image") !=
                std::string::npos &&
            message.content.find("new per-call approval") != std::string::npos)
            sawRecovery = true;
    }
    CHECK(sawRecovery);
}

void testTextModerationGetsTargetedGuidance() {
    auto scenario =
        startScenario(R"({"status":"failed","reply":"input text sensitive"})", true, true, true);
    waitForStatus(scenario, MaiSpecialistTaskStatus::Failed);
    for (int attempt = 0; attempt < 100 && scenario.fakeModel->requestCount() == 0; ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(scenario.fakeModel->requestCount() > 0);
    if (scenario.fakeModel->requestCount() == 0) return;
    bool targeted = false;
    for (const MaiModelMessage& message : scenario.fakeModel->lastRequest().messages) {
        if (message.role == MaiModelRole::System &&
            message.content.find("provider labeled the text input as sensitive") !=
                std::string::npos)
            targeted = true;
    }
    CHECK(targeted);
}

void testAmbiguousModerationGivesConcreteInputPlan() {
    auto scenario = startScenario(R"({"status":"failed","reply":"content blocked by moderation"})",
                                  true, true, true);
    waitForStatus(scenario, MaiSpecialistTaskStatus::Failed);
    for (int attempt = 0; attempt < 100 && scenario.fakeModel->requestCount() == 0; ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(scenario.fakeModel->requestCount() > 0);
    if (scenario.fakeModel->requestCount() == 0) return;
    bool targeted = false;
    for (const MaiModelMessage& message : scenario.fakeModel->lastRequest().messages) {
        if (message.role == MaiModelRole::System &&
            message.content.find("not assign it to text or image without evidence") !=
                std::string::npos &&
            message.content.find("light, permitted user-consistent") != std::string::npos &&
            message.content.find("stronger FFmpeg oil-paint-style") != std::string::npos &&
            message.content.find("another provider only after bounded") != std::string::npos &&
            message.content.find("do not launch paid diagnostic generations without approval") !=
                std::string::npos)
            targeted = true;
    }
    CHECK(targeted);
}

void testImageModerationKeepsSameProviderFirst() {
    auto scenario = startScenario(R"({"status":"failed","reply":"reference image sensitive"})",
                                  true, true, true);
    waitForStatus(scenario, MaiSpecialistTaskStatus::Failed);
    for (int attempt = 0; attempt < 100 && scenario.fakeModel->requestCount() == 0; ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(scenario.fakeModel->requestCount() > 0);
    if (scenario.fakeModel->requestCount() == 0) return;
    bool targeted = false;
    for (const MaiModelMessage& message : scenario.fakeModel->lastRequest().messages) {
        if (message.role != MaiModelRole::System) continue;
        const std::size_t light = message.content.find("smallest permitted transformation");
        const std::size_t painterly = message.content.find("stronger FFmpeg oil-paint-style");
        const std::size_t switchProvider = message.content.find("different provider only after");
        if (light != std::string::npos && painterly != std::string::npos &&
            switchProvider != std::string::npos && light < painterly &&
            painterly < switchProvider &&
            message.content.find("Record changed or lost traits") != std::string::npos)
            targeted = true;
    }
    CHECK(targeted);
}

void testPossibleRealPersonDoesNotForceProviderSwitch() {
    auto scenario = startScenario(
        R"({"status":"failed","reply":"input image may contain real person"})", true, true, true);
    waitForStatus(scenario, MaiSpecialistTaskStatus::Failed);
    for (int attempt = 0; attempt < 100 && scenario.fakeModel->requestCount() == 0; ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(scenario.fakeModel->requestCount() > 0);
    if (scenario.fakeModel->requestCount() == 0) return;
    bool targeted = false;
    for (const MaiModelMessage& message : scenario.fakeModel->lastRequest().messages) {
        if (message.role == MaiModelRole::System &&
            message.content.find("does not prove an explicit authorization requirement") !=
                std::string::npos &&
            message.content.find("do not hide the face in an edited image") != std::string::npos &&
            message.content.find("authorized-asset path") != std::string::npos &&
            message.content.find("do not switch providers yet") != std::string::npos &&
            message.content.find("retry the same provider") != std::string::npos &&
            message.content.find("stronger FFmpeg oil-paint-style") != std::string::npos)
            targeted = true;
    }
    CHECK(targeted);
}

void testSeedanceRealFaceUsesOriginalAssetRoute() {
    auto scenario = startScenario(R"({"status":"failed","reply":"image may contain real person"})",
                                  true, true, true, "seedance_video");
    waitForStatus(scenario, MaiSpecialistTaskStatus::Failed);
    for (int attempt = 0; attempt < 100 && scenario.fakeModel->requestCount() == 0; ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(scenario.fakeModel->requestCount() > 0);
    if (scenario.fakeModel->requestCount() == 0) return;
    bool targeted = false;
    for (const MaiModelMessage& message : scenario.fakeModel->lastRequest().messages) {
        if (message.role == MaiModelRole::System &&
            message.content.find("ark_assets upload_image") != std::string::npos &&
            message.content.find("get_asset until it is Active") != std::string::npos &&
            message.content.find("Do not default to flipping, oil-painting") != std::string::npos)
            targeted = true;
    }
    CHECK(targeted);
}

void testTechnicalFailureDoesNotGetModerationGuidance() {
    auto scenario =
        startScenario(R"({"status":"failed","reply":"unsupported dimensions"})", true, true, true);
    waitForStatus(scenario, MaiSpecialistTaskStatus::Failed);
    for (int attempt = 0; attempt < 100 && scenario.fakeModel->requestCount() == 0; ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK(scenario.fakeModel->requestCount() > 0);
    if (scenario.fakeModel->requestCount() == 0) return;
    bool genericOnly = false;
    for (const MaiModelMessage& message : scenario.fakeModel->lastRequest().messages) {
        if (message.role == MaiModelRole::System &&
            message.content.find("A delegated specialist has replied") != std::string::npos &&
            message.content.find("This attempt failed") == std::string::npos)
            genericOnly = true;
    }
    CHECK(genericOnly);
}

void testLocalPollingInputErrorDoesNotClaimProviderFailure() {
    auto scenario =
        startScenario(R"({"code":"invalid_input","message":"message is required"})", false);
    waitForStatus(scenario, MaiSpecialistTaskStatus::Failed);
    bool trackingNotice = false;
    for (int attempt = 0; attempt < 100; ++attempt) {
        for (const MaiMessage& message : scenario.agent->listMessages(scenario.sessionId)) {
            if (message.text().find("Video task status unavailable. message is required") == 0)
                trackingNotice = true;
            CHECK(message.text().find("Video task failed.") == std::string::npos);
        }
        if (trackingNotice) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(trackingNotice);
}

}  // namespace

int main() {
    testFailureVisibleEvenWhenMainModelFails();
    testAuthorizationFailureIsTerminalWithoutMainModel();
    testRateLimitRemainsRetryable();
    testBalanceErrorHandsTaskBackToMainWithoutExposingFailure();
    testRepeatedRateLimitEventuallyNotifiesMainConversation();
    testCompletedWithoutOutputShowsFailure();
    testProviderFailedStatusNotifiesMainConversation();
    testFailedSpecialistAddsRecoveryInstructionToMainModel();
    testTextModerationGetsTargetedGuidance();
    testAmbiguousModerationGivesConcreteInputPlan();
    testImageModerationKeepsSameProviderFirst();
    testPossibleRealPersonDoesNotForceProviderSwitch();
    testSeedanceRealFaceUsesOriginalAssetRoute();
    testTechnicalFailureDoesNotGetModerationGuidance();
    testLocalPollingInputErrorDoesNotClaimProviderFailure();
    return failures == 0 ? 0 : 1;
}
