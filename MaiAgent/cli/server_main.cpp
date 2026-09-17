#include <cstdio>
#include <cstdlib>
#include <string>

#include "http/server.h"
#include "mai/agent.h"

#include <memory>

using namespace mai;

namespace {

void usage() {
  std::printf(
      "用法: maiagent-server [选项]\n"
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

const char* env_or_empty(const char* name) {
  const char* v = std::getenv(name);
  return v ? v : "";
}

}  // namespace

int main(int argc, char** argv) {
  http::ServerOptions opts;
  std::string model_url;
  std::string model_key = env_or_empty("MAIAGENT_API_KEY");
  std::string model_name = "glm-5.3";

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    const bool has_next = (i + 1) < argc;
    if (a == "--help" || a == "-h") {
      usage();
      return 0;
    } else if (a == "--port" && has_next) {
      opts.port = std::atoi(argv[++i]);
    } else if (a == "--host" && has_next) {
      opts.host = argv[++i];
    } else if (a == "--model-url" && has_next) {
      model_url = argv[++i];
    } else if (a == "--model-key" && has_next) {
      model_key = argv[++i];
    } else if (a == "--model" && has_next) {
      model_name = argv[++i];
    } else {
      std::fprintf(stderr, "maiagent: 无法识别的参数 %s\n", a.c_str());
      usage();
      return 2;
    }
  }

  std::unique_ptr<ModelClient> model;
  if (!model_url.empty()) {
    ModelConfig cfg;
    cfg.base_url = model_url;
    cfg.api_key = model_key;
    model = make_model_client(cfg);
  }

  // 装上内置工具。没有工作目录的会话用不了文件类工具（工具层会明确拒绝），
  // 但注册表本身总是装着——要不要给模型看是 ContextBuilder 的事。
  auto tools = std::make_unique<ToolRegistry>();
  register_builtin_tools(*tools);

  Agent::Options agent_opts;
  agent_opts.default_model = model_name;
  Agent agent(make_memory_store(), std::move(model), std::move(tools), agent_opts);
  http::Server server(agent, opts);

  if (!server.bind()) {
    std::fprintf(stderr, "maiagent: 无法绑定 %s:%d\n", opts.host.c_str(), opts.port);
    return 1;
  }
  // 端口必须在开始阻塞之前打出来，否则 port=0 时没人知道它监听在哪。
  std::printf("maiagent listening on %s\n", server.base_url().c_str());
  if (model_url.empty()) {
    std::printf("  模型: 未配置（空转模式，发消息不会有回复）\n");
  } else {
    std::printf("  模型: %s @ %s\n", model_name.c_str(), model_url.c_str());
  }
  std::fflush(stdout);
  return server.serve() ? 0 : 1;
}
