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
        "Usage: maiagent-bridge [options]\n"
        "\n"
        "  --host <addr>        Address to listen on, default 127.0.0.1\n"
        "  --port <n>           Port to listen on; 0 or omitted lets the OS pick one\n"
        "  --model-url <url>    Model base url, for example\n"
        "                         https://open.bigmodel.cn/api/paas/v4   (GLM)\n"
        "                         http://127.0.0.1:11434/v1              (Ollama)\n"
        "  --model-key <key>    API key; MAIAGENT_API_KEY works too\n"
        "  --model <name>       Default model name, default glm-5.3\n"
        "  --db <path>          SQLite file for sessions and messages.\n"
        "                       Omitted means in-memory: everything is gone on exit.\n"
        "  --permission-timeout <ms>\n"
        "                       Milliseconds to wait for user approval. 0 (default) waits\n"
        "                       forever; a timeout counts as a denial. Only needed when\n"
        "                       nobody is there to answer.\n"
        "\n"
        "Without --model-url the bridge idles: the UI starts and sessions can be\n"
        "created, but prompts get no reply.\n");
}

const char* envOrEmpty(const char* name) {
    const char* value = std::getenv(name);
    return value ? value : "";
}

}  // namespace

int main(int argc, char** argv) {
    MaiHttpAdapterOptions options;
    std::string modelUrl;
    std::string modelKey = envOrEmpty("MAIAGENT_API_KEY");
    std::string modelName = "glm-5.3";
    MaiMillis permissionTimeoutMs = 0;
    std::string databasePath;

    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        const bool hasNext = (i + 1) < argc;
        if (argument == "--help" || argument == "-h") {
            usage();
            return 0;
        } else if (argument == "--port" && hasNext) {
            options.port = std::atoi(argv[++i]);
        } else if (argument == "--host" && hasNext) {
            options.host = argv[++i];
        } else if (argument == "--model-url" && hasNext) {
            modelUrl = argv[++i];
        } else if (argument == "--model-key" && hasNext) {
            modelKey = argv[++i];
        } else if (argument == "--model" && hasNext) {
            modelName = argv[++i];
        } else if (argument == "--db" && hasNext) {
            databasePath = argv[++i];
        } else if (argument == "--permission-timeout" && hasNext) {
            permissionTimeoutMs = std::atoll(argv[++i]);
        } else {
            std::fprintf(stderr, "maiagent: unrecognized argument %s\n", argument.c_str());
            usage();
            return 2;
        }
    }

    std::unique_ptr<MaiModelClient> model;
    if (!modelUrl.empty()) {
        MaiModelConfig config;
        config.baseUrl = modelUrl;
        config.apiKey = modelKey;
        model = makeMaiModelClient(config);
    }

    // 装上内置工具。没有工作目录的会话用不了文件类工具（工具层会明确拒绝），
    // 但注册表本身总是装着——要不要给模型看是 MaiContextBuilder 的事。
    auto tools = std::make_unique<MaiToolRegistry>();
    registerMaiBuiltinTools(*tools);

    // 不给 --db 就用内存存储。默认不落盘是有意的：这个 exe 是开发期的桥，
    // 悄悄在用户机器上建个数据库文件不合适。真要留历史就显式给路径。
    std::unique_ptr<MaiSessionStore> store;
    if (databasePath.empty()) {
        store = makeMaiMemoryStore();
    } else {
        auto opened = makeMaiSqliteStore(databasePath);
        if (!opened) {
            std::fprintf(stderr, "maiagent: %s\n", opened.error().message().c_str());
            return 1;
        }
        store = std::move(opened.value());
    }

    MaiAgent::Options agentOptions;
    agentOptions.defaultModel = modelName;
    agentOptions.permissionTimeoutMs = permissionTimeoutMs;
    MaiAgent agent(std::move(store), std::move(model), std::move(tools), agentOptions);
    MaiHttpAdapter server(agent, options);

    if (!server.bind()) {
        std::fprintf(stderr, "maiagent: could not bind %s:%d\n", options.host.c_str(),
                     options.port);
        return 1;
    }
    // 端口必须在开始阻塞之前打出来，否则 port=0 时没人知道它监听在哪。
    std::printf("maiagent listening on %s\n", server.baseUrl().c_str());
    std::printf("  storage: %s\n", databasePath.empty() ? "in-memory (nothing is kept after exit)"
                                                        : databasePath.c_str());
    std::printf("  permissions: write needs approval via POST /api/permission/<id>%s\n",
                permissionTimeoutMs > 0 ? " (with timeout)" : " (no timeout, waits forever)");
    if (modelUrl.empty()) {
        std::printf("  model: not configured (idle mode, prompts get no reply)\n");
    } else {
        std::printf("  model: %s @ %s\n", modelName.c_str(), modelUrl.c_str());
    }
    std::fflush(stdout);
    return server.serve() ? 0 : 1;
}
