#include <array>
#include <cstdio>
#include <future>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "MaiGraphics.h"
#include "MaiGraphicsEffectParser.h"
#include "MaiGraphicsTaskRunner.h"

namespace {

int failures = 0;

#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
            ++failures;                                                     \
        }                                                                   \
    } while (0)

void testSinglePhysicalThreadAndDrain() {
    MaiGraphicsTaskRunner runner;
    CHECK(!runner.isCurrentThread());
    std::vector<int> order;
    std::vector<std::thread::id> threadIds;
    std::vector<std::future<void>> results;
    for (int index = 0; index < 4; ++index) {
        results.push_back(runner.postTask([&, index] {
            CHECK(runner.isCurrentThread());
            order.push_back(index);
            threadIds.push_back(std::this_thread::get_id());
        }));
    }
    runner.stop();
    for (auto& result : results) result.get();
    CHECK(order == std::vector<int>({0, 1, 2, 3}));
    CHECK(threadIds.size() == 4);
    if (threadIds.size() == 4) {
        for (const auto& id : threadIds) CHECK(id == threadIds.front());
        CHECK(threadIds.front() != std::this_thread::get_id());
    }
    bool rejected = false;
    try {
        runner.postTask([] {}).get();
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    CHECK(rejected);
}

void testTaskExceptionDoesNotKillRunner() {
    MaiGraphicsTaskRunner runner;
    bool caught = false;
    try {
        runner.postTask([] { throw std::runtime_error("task failed"); }).get();
    } catch (const std::runtime_error&) {
        caught = true;
    }
    CHECK(caught);
    runner.postTask([] {}).get();
}

void testOffscreenGraphicsAndOwnership() {
    MaiGraphicsTaskRunner runner;
    ags_graphics_t* graphics = nullptr;
    ags_texture_t* texture = nullptr;
    std::array<uint8_t, 4 * 4 * 4> output{};

    runner
        .postTask([&] {
            ags_graphics_t* unsupported = nullptr;
            CHECK(ags_create(&unsupported, AGS_BACKEND_METAL, 4, 4) == AGS_ERROR_NOT_SUPPORTED);
            CHECK(unsupported == nullptr);
            CHECK(ags_create(&graphics, AGS_BACKEND_SOFTWARE, 4, 4) == AGS_SUCCESS);
            CHECK(ags_texture_create(graphics, 1, 1, AGS_COLOR_RGBA8, &texture) == AGS_SUCCESS);
            const uint8_t red[] = {255, 0, 0, 255};
            CHECK(ags_texture_set_image(texture, red, sizeof(red)) == AGS_SUCCESS);
            CHECK(ags_draw_sprite(graphics, texture, {1, 1, 2, 2}, 0.5f) ==
                  AGS_ERROR_INVALID_STATE);
            CHECK(ags_begin_frame(graphics) == AGS_SUCCESS);
            CHECK(ags_begin_frame(graphics) == AGS_ERROR_INVALID_STATE);
            CHECK(ags_clear(graphics, {0, 0, 255, 255}) == AGS_SUCCESS);
            CHECK(ags_draw_sprite(graphics, texture, {1, 1, 2, 2}, 0.5f) == AGS_SUCCESS);
            CHECK(ags_readback(graphics, output.data(), 16, output.size()) ==
                  AGS_ERROR_INVALID_STATE);
            CHECK(ags_end_frame(graphics) == AGS_SUCCESS);
            CHECK(ags_readback(graphics, output.data(), 16, output.size()) == AGS_SUCCESS);
        })
        .get();

    CHECK(ags_begin_frame(graphics) == AGS_ERROR_WRONG_THREAD);
    CHECK(ags_texture_destroy(texture) == AGS_ERROR_WRONG_THREAD);
    CHECK(output[0] == 0 && output[1] == 0 && output[2] == 255 && output[3] == 255);
    const size_t center = (1 * 4 + 1) * 4;
    CHECK(output[center] == 128 && output[center + 1] == 0 && output[center + 2] == 128 &&
          output[center + 3] == 255);

    runner
        .postTask([&] {
            CHECK(ags_destroy(graphics) == AGS_ERROR_INVALID_STATE);
            CHECK(ags_texture_destroy(texture) == AGS_SUCCESS);
            CHECK(ags_resize(graphics, 2, 2) == AGS_SUCCESS);
            CHECK(ags_readback(graphics, output.data(), 4, output.size()) ==
                  AGS_ERROR_INVALID_ARGUMENT);
            CHECK(ags_destroy(graphics) == AGS_SUCCESS);
        })
        .get();
}

void testEffectTechniqueAndPassParsing() {
    const std::string source = R"(
#include "colors.effect"
uniform float4 tint = {1.0, 0.5, 0.5, 1.0};
sampler_state linear_sampler { Filter = Linear; AddressU = Clamp; };
float4 draw_vertex(float4 input) {
    if (input.x > 0) { return input; }
    return float4(0, 0, 0, 1);
}
float4 draw_pixel(float4 input) { return input * tint; }
technique Draw {
    pass Main {
        vertex_shader = draw_vertex(input);
        pixel_shader = draw_pixel(input);
    }
}
technique DrawAgain {
    pass {
        vertex_program = draw_vertex(input);
        pixel_program = draw_pixel(input);
    }
}
)";
    MaiGraphicsEffectParser parser;
    const auto parsed = parser.parse(source);
    CHECK(parsed.isOk());
    CHECK(parsed.effect.includes == std::vector<std::string>({"colors.effect"}));
    CHECK(parsed.effect.techniques.size() == 2);
    if (parsed.effect.techniques.size() == 2) {
        CHECK(parsed.effect.techniques[0].name == "Draw");
        CHECK(parsed.effect.techniques[0].passes.size() == 1);
        if (!parsed.effect.techniques[0].passes.empty()) {
            const auto& pass = parsed.effect.techniques[0].passes[0];
            CHECK(pass.name == "Main");
            CHECK(pass.vertexShader == "draw_vertex");
            CHECK(pass.pixelShader == "draw_pixel");
        }
        CHECK(parsed.effect.techniques[1].passes.size() == 1);
    }
    const auto missingSemicolon =
        parser.parse("technique Draw { pass { vertex_shader = VS(x) pixel_shader = PS(x); } }");
    CHECK(!missingSemicolon.isOk());
    CHECK(missingSemicolon.line > 0 && missingSemicolon.column > 0);
    CHECK(missingSemicolon.effect.techniques.empty());
    CHECK(!parser.parse("technique Draw { pass { vertex_shader = VS(x); } }").isOk());
    CHECK(!parser.parse("/* unterminated").isOk());
}

}  // namespace

int main() {
    testSinglePhysicalThreadAndDrain();
    testTaskExceptionDoesNotKillRunner();
    testOffscreenGraphicsAndOwnership();
    testEffectTechniqueAndPassParsing();
    if (!failures) std::puts("all graphics tests passed");
    return failures ? 1 : 0;
}
