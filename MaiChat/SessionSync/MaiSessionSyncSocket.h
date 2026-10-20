// Shared WebSocket transport for Desktop, iOS, and Android Session sync.
#pragma once

#include <functional>
#include <memory>
#include <string>

class MaiSessionSyncSocket final {
public:
    struct Configuration {
        std::string url;
        std::string bearer;
        std::string caPem;
        std::string proxyUrl;
    };

    enum class Event { Connected, Disconnected, Message, Error };
    using Handler = std::function<void(Event, const std::string&)>;

    // The handler runs on the socket thread. Callers must dispatch UI work and
    // must not destroy the socket from inside the handler. No callback runs
    // after stop() returns. A stopped instance may be started again.
    explicit MaiSessionSyncSocket(Handler handler);
    ~MaiSessionSyncSocket();

    MaiSessionSyncSocket(const MaiSessionSyncSocket&) = delete;
    MaiSessionSyncSocket& operator=(const MaiSessionSyncSocket&) = delete;

    // Only wss:// is accepted. proxyUrl may be empty or an HTTP CONNECT URL.
    // Returns false if the configuration is invalid or already running.
    bool start(Configuration configuration);
    // Supplies a refreshed account access token for the next connection.
    bool updateBearer(std::string bearer);
    // Queues one text frame only while connected. Delivery requires a server
    // response; true does not mean the device executed the request.
    bool send(std::string message);
    void stop();
    bool isConnected() const;

private:
    struct Runtime;
    std::shared_ptr<Runtime> mRuntime;
};
