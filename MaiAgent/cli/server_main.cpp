#include <cstdio>
#include <cstdlib>
#include <string>

#include "http/server.h"
#include "mai/agent/thread.h"

using namespace mai::agent;

int main(int argc, char** argv) {
  http::Options opts;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--port" && i + 1 < argc) opts.port = std::atoi(argv[++i]);
    else if (a == "--host" && i + 1 < argc) opts.host = argv[++i];
  }

  Agent agent(make_memory_store());
  http::Server server(agent, opts);

  // 先建两条会话，好让 UI 一接上就有东西显示，能立刻判断通没通。
  agent.submit(OpCreateSession{"", "MaiAgent 第一条会话", ""});
  agent.submit(OpCreateSession{"", "第二条", ""});

  if (!server.bind()) {
    std::fprintf(stderr, "maiagent: failed to bind %s:%d\n", opts.host.c_str(), opts.port);
    return 1;
  }
  // 端口必须在开始阻塞之前打出来，否则 port=0 时没人知道它监听在哪。
  std::printf("maiagent listening on %s\n", server.base_url().c_str());
  std::fflush(stdout);
  return server.serve() ? 0 : 1;
}
