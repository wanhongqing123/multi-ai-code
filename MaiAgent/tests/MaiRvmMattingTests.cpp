#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "MaiRvmMatting.h"

static int failures = 0;
#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
            ++failures;                                                     \
        }                                                                   \
    } while (0)

int main() {
    const auto missing = MaiRvmMattingSession::open("missing-rvm-model.onnx");
    CHECK(!missing);
    CHECK(missing.error().code() == MaiErrorCode::NotFound);

    const char* runtime = std::getenv("MAI_ONNXRUNTIME_LIBRARY");
    if (runtime == nullptr || *runtime == '\0') {
        std::printf("RVM runtime inference skipped: MAI_ONNXRUNTIME_LIBRARY is unset\n");
        return failures == 0 ? 0 : 1;
    }
    auto opened = MaiRvmMattingSession::open(MAI_RVM_MODEL_PATH, runtime);
    CHECK(opened);
    if (!opened) {
        std::printf("RVM open error: %s\n", opened.error().message().c_str());
        return 1;
    }
    auto session = std::move(opened.value());
    CHECK(session->runtimeVersion().find("1.26") == 0);
    constexpr int width = 128;
    constexpr int height = 128;
    std::vector<float> frame(3 * width * height, 0.5f);
    auto first = session->matte(frame.data(), width, height);
    CHECK(first);
    if (!first) return 1;
    CHECK(first.value().size() == width * height);
    for (const float value : first.value()) CHECK(std::isfinite(value) && value >= 0 && value <= 1);
    auto next = session->infer(frame.data(), width, height);
    CHECK(next);
    if (next) {
        CHECK(next.value().alpha.size() == width * height);
        CHECK(next.value().foregroundRgbPlanar.size() == 3 * width * height);
    }
    CHECK(!session->reset());
    auto again = session->matte(frame.data(), width, height);
    CHECK(again);
    if (again) {
        for (std::size_t index = 0; index < first.value().size(); ++index)
            CHECK(std::abs(first.value()[index] - again.value()[index]) < 0.0001f);
    }
    CHECK(!session->matte(frame.data(), 0, height));
    return failures == 0 ? 0 : 1;
}
