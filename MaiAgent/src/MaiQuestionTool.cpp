#include <json.hpp>

#include <string>
#include <vector>

#include "MaiIdGenerator.h"
#include "MaiTime.h"
#include "MaiTool.h"

// question：中途问用户一句，等回答，然后接着干。
//
// ── 什么时候该用 ────────────────────────────────────────────────
//
// 不用它其实也能凑合：把问题写在回答里，这一轮结束，用户回一句，下一轮继续。
// 差别在于**模型丢了现场**——它已经读了十个文件、跑了三次测试，那些结论还在上下文里，
// 但它得重新判断进行到哪一步。有这个工具的话那一轮不结束，问完接着干。
//
// 所以适用面很窄：**只在「这一步选错了后面全白做」的时候问**。
// 什么都问一句的模型比什么都不问的更烦人，描述里把这条写给模型看。

namespace {

using json = nlohmann::json;

json parseArguments(const std::string& raw) {
    json parsed = json::parse(raw, nullptr, false);
    return parsed.is_object() ? parsed : json::object();
}

class QuestionTool final : public MaiTool {
public:
    std::string name() const override {
        return "question";
    }

    std::string description() const override {
        return "Ask the user one question and wait for the answer, then keep going in the same "
               "turn. Use it only when getting this wrong would waste everything that follows, "
               "such as which of two designs to build. Do not use it for things you can decide "
               "yourself or find out by reading the code: asking about everything is worse than "
               "asking about nothing.";
    }

    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{)"
               R"("question":{"type":"string","description":"The question, in the user's language"},)"
               R"("options":{"type":"array","items":{"type":"string"},)"
               R"("description":"Suggested answers, if there are a few obvious ones. The user can still answer with anything."}},)"
               R"("required":["question"]})";
    }

    // 不改任何东西，所以不走权限闸——它自己就是在等用户说话。
    // 再套一层「要不要允许提问」只会多一次点击。
    MaiToolResult execute(const std::string& raw, const MaiToolContext& context) override {
        const json args = parseArguments(raw);
        const std::string question = args.value("question", std::string{});
        if (question.empty()) {
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "missing required parameter: question");
        }
        if (context.questions == nullptr || !context.announceQuestion) {
            // 宿主没接问答。**明说问不了**，别干等到超时——
            // 干等的话用户看到的是「卡住了」，而模型收到的是「超时」，两边都不知道真相。
            return MaiToolResult::failure(
                MaiErrorCode::NotSupported,
                "this app cannot ask the user questions. Decide for yourself, or explain the "
                "choice in your answer and stop.");
        }

        MaiQuestionRequest request;
        request.id = MaiIdGenerator::newQuestionId();
        request.sessionId = context.sessionId;
        request.messageId = context.messageId;
        request.partId = context.partId;
        request.question = question;
        if (args.contains("options") && args["options"].is_array()) {
            for (const auto& option : args["options"]) {
                if (option.is_string()) request.options.push_back(option.get<std::string>());
            }
        }
        request.asked = MaiTime::getCurrentTime();

        static const std::atomic<bool> kNeverCanceled{false};
        const std::atomic<bool>& cancel =
            context.cancel != nullptr ? *context.cancel : kNeverCanceled;
        const std::string answer =
            context.questions->ask(request, context.announceQuestion, cancel);

        if (answer.empty()) {
            // 没等到。**不能编一个答案**，也不能说成「用户拒绝了」——
            // 用户可能根本没看见这次提问。说的必须是真的，模型才不会基于假前提往下做。
            return MaiToolResult::failure(
                MaiErrorCode::Canceled,
                "the user did not answer (the turn was interrupted or the question timed out)");
        }
        return MaiToolResult::success(answer);
    }
};

}  // namespace

std::unique_ptr<MaiTool> makeMaiQuestionTool() {
    return std::make_unique<QuestionTool>();
}
