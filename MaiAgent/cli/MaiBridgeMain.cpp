// maiagent-bridge：把核心跑起来，并通过 HTTP 适配器暴露给 Electron 界面。
//
// 叫 bridge 不叫 server，是因为它不是这个项目的产物，只是座桥：
// 界面是 JS 写的、调不了 C++，所以要有个进程夹在中间。Qt 界面就位之后
// 这个 exe 就没用了——那边直接链 maiagent 库，进程内调用。
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#include "MaiAgent.h"
#include "MaiHttpAdapter.h"

namespace {

void usage() {
    std::printf(
        "用法: maiagent-bridge [选项]\n"
        "\n"
        "  --host <addr>        监听地址，默认 127.0.0.1\n"
        "  --port <n>           监听端口，0 或省略则由系统分配\n"
        "  --model-url <url>    模型的 base url，例如\n"
        "                         https://open.bigmodel.cn/api/paas/v4   (GLM)\n"
        "                         http://127.0.0.1:11434/v1              (Ollama)\n"
        "  --model-key <key>    API key；也可用环境变量 MAIAGENT_API_KEY\n"
        "  --model <name>       默认模型名，默认 glm-5.3\n"
        "\n"
        "不给 --model-url 就是空转模式：界面能起、会话能建，但发消息不会有回复。\n");
}

const char* envOrEmpty(const char* name) {
    const char* v = std::getenv(name);
    return v ? v : "";
}

}  // namespace

int main(int argc, char** argv) {
    MaiHttpAdapterOptions opts;
    std::string modelUrl;
    std::string modelKey = envOrEmpty("MAIAGENT_API_KEY");
    std::string modelName = "glm-5.3";

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool hasNext = (i + 1) < argc;
        if (a == "--help" || a == "-h") {
            usage();
            return 0;
        } else if (a == "--port" && hasNext) {
            opts.port = std::atoi(argv[++i]);
        } else if (a == "--host" && hasNext) {
            opts.host = argv[++i];
        } else if (a == "--model-url" && hasNext) {
            modelUrl = argv[++i];
        } else if (a == "--model-key" && hasNext) {
            modelKey = argv[++i];
        } else if (a == "--model" && hasNext) {
            modelName = argv[++i];
        } else {
            std::fprintf(stderr, "maiagent: 无法识别的参数 %s\n", a.c_str());
            usage();
            return 2;
        }
    }

    std::unique_ptr<MaiModelClient> model;
    if (!modelUrl.empty()) {
        MaiModelConfig cfg;
        cfg.baseUrl = modelUrl;
        cfg.apiKey = modelKey;
        model = makeMaiModelClient(cfg);
    }

    // 装上内置工具。没有工作目录的会话用不了文件类工具（工具层会明确拒绝），
    // 但注册表本身总是装着——要不要给模型看是 MaiContextBuilder 的事。
    auto tools = std::make_unique<MaiToolRegistry>();
    registerMaiBuiltinTools(*tools);

    MaiAgent::Options agentOptions;
    agentOptions.defaultModel = modelName;
    MaiAgent agent(makeMaiMemoryStore(), std::move(model), std::move(tools), agentOptions);
    MaiHttpAdapter server(agent, opts);

    if (!server.bind()) {
        std::fprintf(stderr, "maiagent: 无法绑定 %s:%d\n", opts.host.c_str(), opts.port);
        return 1;
    }
    // 端口必须在开始阻塞之前打出来，否则 port=0 时没人知道它监听在哪。
    std::printf("maiagent listening on %s\n", server.baseUrl().c_str());
    if (modelUrl.empty()) {
        std::printf("  模型: 未配置（空转模式，发消息不会有回复）\n");
    } else {
        std::printf("  模型: %s @ %s\n", modelName.c_str(), modelUrl.c_str());
    }
    std::fflush(stdout);
    return server.serve() ? 0 : 1;
}
