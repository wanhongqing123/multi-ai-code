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
    StaticMediaTool(std::string result, bool success)
        : mResult(std::move(result)), mSuccess(success) {}

    std::string name() const override {
        return "wan_video";
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
};

struct Scenario {
    std::unique_ptr<MaiAgent> agent;
    MaiSpecialistTaskStore* store = nullptr;
    std::string sessionId;
    std::string taskId;
};

Scenario startScenario(std::string result, bool withModel, bool success = false) {
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
    task.specialistName = "wan_video";
    task.providerTaskId = "provider-task";
    task.intent = "Create a video";
    task.created = MaiTime::getCurrentTime();
    CHECK(!store->insertSpecialistTask(task));
    scenario.taskId = task.id;

    std::unique_ptr<MaiModelClient> model;
    if (withModel) {
        auto fake = std::make_unique<MaiFakeModelClient>();
        MaiFakeModelClient::Turn reply;
        reply.error = MaiError::make(MaiErrorCode::Network, "Main model is offline");
        fake->setRepeatingTurn(reply);
        model = std::move(fake);
    }
    auto tools = std::make_unique<MaiToolRegistry>();
    tools->add(std::make_unique<StaticMediaTool>(std::move(result), success));
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
            if (message.text().find("Video task failed. Access denied") == 0) visible = true;
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

void testCompletedWithoutOutputShowsFailure() {
    auto scenario = startScenario(R"({"status":"succeeded","reply":"Done"})", false, true);
    waitForStatus(scenario, MaiSpecialistTaskStatus::Failed);
    bool visible = false;
    for (int attempt = 0; attempt < 100; ++attempt) {
        for (const MaiMessage& message : scenario.agent->listMessages(scenario.sessionId)) {
            if (message.text().find("Video task failed. The provider completed the task") == 0)
                visible = true;
        }
        if (visible) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(visible);
}

}  // namespace

int main() {
    testFailureVisibleEvenWhenMainModelFails();
    testAuthorizationFailureIsTerminalWithoutMainModel();
    testRateLimitRemainsRetryable();
    testCompletedWithoutOutputShowsFailure();
    return failures == 0 ? 0 : 1;
}
