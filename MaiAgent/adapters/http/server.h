#pragma once
#include <memory>
#include <string>

#include "mai/agent.h"

namespace mai::http {

struct ServerOptions {
  std::string host = "127.0.0.1";
  int port = 0;   // 0 = 让系统选一个空闲端口，启动后用 port() 取实际值
};

// 把核心包成 REST + SSE，喂现有的 Electron UI。
//
// 这一层是**唯一**出现 JSON 的地方。核心里全是原生结构体，
// 进来的请求在这里解析成 Op，出去的事件在这里序列化成 SSE。
class Server {
 public:
  Server(Agent& agent, ServerOptions opts);
  ~Server();
  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  // bind 和阻塞循环必须分开：port=0 时端口是系统分配的，
  // 调用方要在开始阻塞之前就能拿到它（打印出来、写进文件给 UI 用）。
  bool bind();     // 成功后 port() 即可用
  bool serve();    // 阻塞直到 stop()
  void stop();

  int port() const;          // 实际监听到的端口
  std::string base_url() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mai::http
