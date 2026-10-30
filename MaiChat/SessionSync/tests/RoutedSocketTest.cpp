// 本地 TLS fixture 验证备用 IP 连接和断线后的操作不重放。
#include "MaiSessionSyncSocket.h"
#include <MaiThread.h>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <thread>

void MaiThread::setCurrentName(const std::string&) {}

int main(int argc, char** argv) {
    if (argc != 4) return 2;
    std::ifstream in(argv[2]);
    std::string ca((std::istreambuf_iterator<char>(in)), {});
    std::atomic<int> connected{0};
    MaiSessionSyncSocket socket([&](MaiSessionSyncSocket::Event event, const std::string&) {
        if (event == MaiSessionSyncSocket::Event::Connected) ++connected;
    });
    MaiSessionSyncSocket::Configuration config{
        std::string("wss://ichat.life:") + argv[1] + "/v1/session-sync/ws",
        "local-test-token", ca, "http://127.0.0.1:1"};
    if (!socket.start(config)) return 3;
    for (int i = 0; i < 300 && connected.load() < 1; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    if (std::string(argv[3]) == "reject") {
        const bool rejected = connected.load() == 0;
        socket.stop();
        return rejected ? 0 : 4;
    }
    if (connected.load() != 1 || !socket.send("{\"op\":\"send\"}")) return 5;
    for (int i = 0; i < 500 && connected.load() < 2; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const bool reconnected = connected.load() >= 2;
    socket.stop();
    return reconnected ? 0 : 6;
}
