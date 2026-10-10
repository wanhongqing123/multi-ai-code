#include "MaiRvmMatting.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <onnxruntime_c_api.h>

#include "MaiFilePath.h"
#include "MaiFileSystem.h"

namespace {

constexpr std::array<const char*, 6> kInputNames = {"src", "r1i", "r2i",
                                                    "r3i", "r4i", "downsample_ratio"};
constexpr std::array<const char*, 6> kOutputNames = {"fgr", "pha", "r1o", "r2o", "r3o", "r4o"};

#if defined(_WIN32)
using MaiOrtLibrary = HMODULE;
MaiOrtLibrary openRuntime(const std::string& path) {
    return LoadLibraryW(MaiFilePath::fromUtf8(path).value().c_str());
}
void* runtimeSymbol(MaiOrtLibrary library) {
    return reinterpret_cast<void*>(GetProcAddress(library, "OrtGetApiBase"));
}
void closeRuntime(MaiOrtLibrary library) {
    if (library != nullptr) FreeLibrary(library);
}
void* linkedRuntimeSymbol() {
    HMODULE library = GetModuleHandleW(L"onnxruntime.dll");
    return library != nullptr ? runtimeSymbol(library) : nullptr;
}
#else
using MaiOrtLibrary = void*;
MaiOrtLibrary openRuntime(const std::string& path) {
    return dlopen(MaiFilePath::fromUtf8(path).value().c_str(), RTLD_NOW | RTLD_LOCAL);
}
void* runtimeSymbol(MaiOrtLibrary library) {
    return dlsym(library, "OrtGetApiBase");
}
void closeRuntime(MaiOrtLibrary library) {
    if (library != nullptr) dlclose(library);
}
void* linkedRuntimeSymbol() {
    return dlsym(RTLD_DEFAULT, "OrtGetApiBase");
}
#endif

}  // namespace

class MaiRvmMattingSession::Impl {
public:
    ~Impl() {
        if (api != nullptr) {
            for (OrtValue*& state : states) {
                if (state != nullptr) api->ReleaseValue(state);
            }
            if (memory != nullptr) api->ReleaseMemoryInfo(memory);
            if (session != nullptr) api->ReleaseSession(session);
            if (options != nullptr) api->ReleaseSessionOptions(options);
            if (environment != nullptr) api->ReleaseEnv(environment);
        }
        closeRuntime(library);
    }

    MaiError check(OrtStatus* status, const char* action) const {
        if (status == nullptr) return MaiError::ok();
        const std::string message = api->GetErrorMessage(status);
        api->ReleaseStatus(status);
        return MaiError::make(MaiErrorCode::Internal, std::string(action) + ": " + message);
    }

    MaiError reset() {
        std::array<OrtValue*, 4> replacements{};
        constexpr std::array<int64_t, 4> shape = {1, 1, 1, 1};
        for (OrtValue*& state : replacements) {
            const MaiError error =
                check(api->CreateTensorWithDataAsOrtValue(
                          memory, &zero, sizeof(zero), shape.data(), shape.size(),
                          ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &state),
                      "create recurrent state");
            if (error) {
                for (OrtValue* replacement : replacements) {
                    if (replacement != nullptr) api->ReleaseValue(replacement);
                }
                return error;
            }
        }
        for (OrtValue* state : states) {
            if (state != nullptr) api->ReleaseValue(state);
        }
        states = replacements;
        return MaiError::ok();
    }

    MaiOrtLibrary library = nullptr;
    const OrtApi* api = nullptr;
    const OrtApiBase* apiBase = nullptr;
    OrtEnv* environment = nullptr;
    OrtSessionOptions* options = nullptr;
    OrtSession* session = nullptr;
    OrtMemoryInfo* memory = nullptr;
    std::array<OrtValue*, 4> states{};
    float zero = 0;
};

MaiResult<std::unique_ptr<MaiRvmMattingSession>> MaiRvmMattingSession::open(
    const std::string& modelPath, const std::string& runtimePath, const void* apiBase) {
    const MaiFilePath model = MaiFilePath::fromUtf8(modelPath);
    if (model.isEmpty() || !MaiFileSystem::exists(model) || MaiFileSystem::isDirectory(model))
        return {MaiErrorCode::NotFound, "RVM model file is missing"};

    auto result = std::unique_ptr<MaiRvmMattingSession>(new MaiRvmMattingSession());
    result->mImpl = std::make_unique<Impl>();
    Impl& state = *result->mImpl;
    void* symbol = nullptr;
    if (apiBase != nullptr) {
        state.apiBase = static_cast<const OrtApiBase*>(apiBase);
    } else if (runtimePath.empty()) {
        symbol = linkedRuntimeSymbol();
    } else {
        state.library = openRuntime(runtimePath);
        if (state.library != nullptr) symbol = runtimeSymbol(state.library);
    }
    if (symbol == nullptr && state.apiBase == nullptr)
        return {MaiErrorCode::NotConfigured,
                "ONNX Runtime 1.26 must be bundled and supplied by the MaiChat host"};
    if (state.apiBase == nullptr) {
        using GetApiBase = const OrtApiBase*(ORT_API_CALL*)();
        state.apiBase = reinterpret_cast<GetApiBase>(symbol)();
    }
    state.api = state.apiBase != nullptr ? state.apiBase->GetApi(ORT_API_VERSION) : nullptr;
    if (state.api == nullptr)
        return {MaiErrorCode::NotSupported, "bundled ONNX Runtime does not support API 26"};
    const OrtApi& api = *state.api;
    MaiError error =
        state.check(api.CreateEnv(ORT_LOGGING_LEVEL_WARNING, "MaiRvmMatting", &state.environment),
                    "create ONNX Runtime environment");
    if (error) return error;
    error = state.check(api.CreateSessionOptions(&state.options), "create session options");
    if (error) return error;
    error = state.check(
        api.CreateSession(state.environment, model.value().c_str(), state.options, &state.session),
        "load RVM model");
    if (error) return error;
    error =
        state.check(api.CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &state.memory),
                    "create CPU tensor memory");
    if (error) return error;
    error = state.reset();
    if (error) return error;
    return result;
}

MaiRvmMattingSession::~MaiRvmMattingSession() = default;

MaiError MaiRvmMattingSession::reset() {
    return mImpl->reset();
}

std::string MaiRvmMattingSession::runtimeVersion() const {
    return mImpl->apiBase->GetVersionString();
}

MaiResult<MaiRvmMattingResult> MaiRvmMattingSession::infer(float* rgbPlanar, int width, int height,
                                                           float downsampleRatio) {
    if (rgbPlanar == nullptr || width <= 0 || height <= 0 ||
        width > std::numeric_limits<int>::max() / height / 3 || !std::isfinite(downsampleRatio) ||
        downsampleRatio <= 0 || downsampleRatio > 1)
        return {MaiErrorCode::InvalidInput, "invalid RVM frame shape or downsample ratio"};
    Impl& state = *mImpl;
    const OrtApi& api = *state.api;
    const std::array<int64_t, 4> frameShape = {1, 3, height, width};
    const std::array<int64_t, 1> ratioShape = {1};
    OrtValue* frameInput = nullptr;
    OrtValue* ratioInput = nullptr;
    MaiError error = state.check(
        api.CreateTensorWithDataAsOrtValue(
            state.memory, rgbPlanar, static_cast<std::size_t>(width) * height * 3 * sizeof(float),
            frameShape.data(), frameShape.size(), ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &frameInput),
        "create source tensor");
    if (error) return error;
    error =
        state.check(api.CreateTensorWithDataAsOrtValue(
                        state.memory, &downsampleRatio, sizeof(downsampleRatio), ratioShape.data(),
                        ratioShape.size(), ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &ratioInput),
                    "create downsample ratio tensor");
    if (error) {
        api.ReleaseValue(frameInput);
        return error;
    }
    const std::array<const OrtValue*, 6> inputs = {
        frameInput, state.states[0], state.states[1], state.states[2], state.states[3], ratioInput};
    std::array<OrtValue*, 6> outputs{};
    error = state.check(api.Run(state.session, nullptr, kInputNames.data(), inputs.data(),
                                inputs.size(), kOutputNames.data(), outputs.size(), outputs.data()),
                        "run RVM inference");
    api.ReleaseValue(frameInput);
    api.ReleaseValue(ratioInput);
    if (error) {
        for (OrtValue* output : outputs) {
            if (output != nullptr) api.ReleaseValue(output);
        }
        return error;
    }
    OrtTensorTypeAndShapeInfo* shape = nullptr;
    error = state.check(api.GetTensorTypeAndShape(outputs[1], &shape), "read alpha shape");
    std::size_t elements = 0;
    if (!error)
        error =
            state.check(api.GetTensorShapeElementCount(shape, &elements), "read alpha pixel count");
    if (shape != nullptr) api.ReleaseTensorTypeAndShapeInfo(shape);
    if (!error && elements != static_cast<std::size_t>(width) * height)
        error = MaiError::make(MaiErrorCode::Protocol, "RVM returned an unexpected alpha size");
    OrtTensorTypeAndShapeInfo* foregroundShape = nullptr;
    std::size_t foregroundElements = 0;
    if (!error)
        error = state.check(api.GetTensorTypeAndShape(outputs[0], &foregroundShape),
                            "read foreground shape");
    if (!error)
        error = state.check(api.GetTensorShapeElementCount(foregroundShape, &foregroundElements),
                            "read foreground pixel count");
    if (foregroundShape != nullptr) api.ReleaseTensorTypeAndShapeInfo(foregroundShape);
    if (!error && foregroundElements != elements * 3)
        error =
            MaiError::make(MaiErrorCode::Protocol, "RVM returned an unexpected foreground size");
    void* alphaData = nullptr;
    void* foregroundData = nullptr;
    if (!error)
        error = state.check(api.GetTensorMutableData(outputs[1], &alphaData), "read alpha pixels");
    if (!error)
        error = state.check(api.GetTensorMutableData(outputs[0], &foregroundData),
                            "read foreground pixels");
    if (error || alphaData == nullptr || foregroundData == nullptr) {
        for (OrtValue* output : outputs) {
            if (output != nullptr) api.ReleaseValue(output);
        }
        return error ? error : MaiError::make(MaiErrorCode::Protocol, "RVM returned no pixels");
    }
    const float* alpha = static_cast<const float*>(alphaData);
    const float* foreground = static_cast<const float*>(foregroundData);
    MaiRvmMattingResult result;
    result.alpha.assign(alpha, alpha + elements);
    result.foregroundRgbPlanar.assign(foreground, foreground + foregroundElements);
    api.ReleaseValue(outputs[0]);
    api.ReleaseValue(outputs[1]);
    for (std::size_t index = 0; index < state.states.size(); ++index) {
        api.ReleaseValue(state.states[index]);
        state.states[index] = outputs[index + 2];
    }
    return result;
}

MaiResult<std::vector<float>> MaiRvmMattingSession::matte(float* rgbPlanar, int width, int height,
                                                          float downsampleRatio) {
    MaiResult<MaiRvmMattingResult> result = infer(rgbPlanar, width, height, downsampleRatio);
    if (!result) return result.error();
    return std::move(result.value().alpha);
}
