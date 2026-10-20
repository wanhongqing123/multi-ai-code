#include "MaiSessionSyncSocket.h"

#include <MaiThread.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

#include <websocketpp/client.hpp>
#include <websocketpp/config/asio_client.hpp>

namespace {

using Client = websocketpp::client<websocketpp::config::asio_tls_client>;

bool isValidConfiguration(const MaiSessionSyncSocket::Configuration& value) {
    const websocketpp::uri uri(value.url);
    return value.url.rfind("wss://", 0) == 0 && uri.get_valid() && !uri.get_host().empty() && value.url.size() < 4096 &&
           value.url.find_first_of("\r\n") == std::string::npos && !value.bearer.empty() &&
           value.bearer.size() <= 8192 && value.bearer.find_first_of("\r\n") == std::string::npos &&
           !value.caPem.empty() && value.proxyUrl.size() < 2048 &&
           value.proxyUrl.find_first_of("\r\n") == std::string::npos &&
           (value.proxyUrl.empty() || value.proxyUrl.rfind("http://", 0) == 0);
}

}  // namespace

struct MaiSessionSyncSocket::Runtime {
    explicit Runtime(Handler callback) : handler(std::move(callback)) {}

    mutable std::mutex mutex;
    std::condition_variable wake;
    std::thread worker;
    Configuration configuration;
    Handler handler;
    std::shared_ptr<Client> client;
    websocketpp::connection_hdl connection;
    bool stopped = true;
    bool connected = false;

    void notify(Event event, const std::string& detail = {}) {
        try {
            if (handler) handler(event, detail);
        } catch (...) { /* A platform callback cannot terminate the socket thread. */
        }
    }

    void loop() {
        MaiThread::setCurrentName("mai-sync-ws");
        while (true) {
            Configuration settings;
            {
                std::lock_guard<std::mutex> guard(mutex);
                if (stopped) return;
                settings = configuration;
            }
            auto endpoint = std::make_shared<Client>();
            endpoint->clear_access_channels(websocketpp::log::alevel::all);
            endpoint->clear_error_channels(websocketpp::log::elevel::all);
            endpoint->init_asio();
            endpoint->set_open_handshake_timeout(10000);
            endpoint->set_close_handshake_timeout(5000);
            endpoint->set_max_message_size(1024 * 1024);
            endpoint->set_tls_init_handler([certificate = settings.caPem](websocketpp::connection_hdl) {
                auto context = websocketpp::lib::make_shared<asio::ssl::context>(asio::ssl::context::tls_client);
                context->set_verify_mode(asio::ssl::verify_peer);
                context->add_certificate_authority(asio::buffer(certificate));
                if (SSL_CTX_set_min_proto_version(context->native_handle(), TLS1_2_VERSION) != 1)
                    throw std::runtime_error("TLS version setup failed");
                return context;
            });
            const std::string host = websocketpp::uri(settings.url).get_host();
            endpoint->set_socket_init_handler(
                [host](websocketpp::connection_hdl, asio::ssl::stream<asio::ip::tcp::socket>& stream) {
                    if (SSL_set_tlsext_host_name(stream.native_handle(), host.c_str()) != 1)
                        throw std::runtime_error("TLS host setup failed");
                    stream.set_verify_callback(asio::ssl::host_name_verification(host));
                });
            endpoint->set_open_handler([this](websocketpp::connection_hdl handle) {
                {
                    std::lock_guard<std::mutex> guard(mutex);
                    this->connection = handle;
                    connected = true;
                }
                notify(Event::Connected);
            });
            endpoint->set_message_handler([this](websocketpp::connection_hdl, Client::message_ptr message) {
                if (message->get_opcode() != websocketpp::frame::opcode::text) return;
                notify(Event::Message, message->get_payload());
            });
            endpoint->set_close_handler([this](websocketpp::connection_hdl) {
                {
                    std::lock_guard<std::mutex> guard(mutex);
                    connected = false;
                }
                notify(Event::Disconnected);
            });
            endpoint->set_fail_handler(
                [this, weak = std::weak_ptr<Client>(endpoint)](websocketpp::connection_hdl handle) {
                    const auto active = weak.lock();
                    if (!active) return;
                    const auto failed = active->get_con_from_hdl(handle);
                    notify(Event::Error, failed->get_ec().message());
                });
            websocketpp::lib::error_code error;
            auto dial = endpoint->get_connection(settings.url, error);
            if (!error) {
                dial->append_header("Authorization", "Bearer " + settings.bearer);
                if (!settings.proxyUrl.empty()) dial->set_proxy(settings.proxyUrl, error);
            }
            if (error) {
                notify(Event::Error, error.message());
            } else {
                {
                    std::lock_guard<std::mutex> guard(mutex);
                    if (stopped) return;
                    client = endpoint;
                }
                endpoint->connect(dial);
                try {
                    endpoint->run();
                } catch (...) {
                    notify(Event::Error, "WebSocket transport failed");
                }
            }
            {
                std::unique_lock<std::mutex> guard(mutex);
                connected = false;
                if (client == endpoint) client.reset();
                if (stopped) return;
                wake.wait_for(guard, std::chrono::seconds(2), [this] { return stopped; });
            }
        }
    }
};

MaiSessionSyncSocket::MaiSessionSyncSocket(Handler handler) : mRuntime(std::make_shared<Runtime>(std::move(handler))) {}

MaiSessionSyncSocket::~MaiSessionSyncSocket() {
    stop();
}

bool MaiSessionSyncSocket::start(Configuration configuration) {
    if (!isValidConfiguration(configuration)) return false;
    const auto runtime = mRuntime;
    std::lock_guard<std::mutex> guard(runtime->mutex);
    if (!runtime->stopped || runtime->worker.joinable()) return false;
    runtime->configuration = std::move(configuration);
    runtime->stopped = false;
    runtime->worker = std::thread([runtime] { runtime->loop(); });
    return true;
}

bool MaiSessionSyncSocket::updateBearer(std::string bearer) {
    if (bearer.empty() || bearer.size() > 8192 || bearer.find_first_of("\r\n") != std::string::npos) return false;
    std::lock_guard<std::mutex> guard(mRuntime->mutex);
    mRuntime->configuration.bearer = std::move(bearer);
    return true;
}

bool MaiSessionSyncSocket::send(std::string message) {
    if (message.empty() || message.size() > 96 * 1024) return false;
    const auto runtime = mRuntime;
    std::shared_ptr<Client> endpoint;
    websocketpp::connection_hdl connection;
    {
        std::lock_guard<std::mutex> guard(runtime->mutex);
        if (runtime->stopped || !runtime->connected || !runtime->client) return false;
        endpoint = runtime->client;
        connection = runtime->connection;
    }
    endpoint->get_io_service().post([endpoint, connection, text = std::move(message)] {
        websocketpp::lib::error_code error;
        endpoint->send(connection, text, websocketpp::frame::opcode::text, error);
    });
    return true;
}

void MaiSessionSyncSocket::stop() {
    const auto runtime = mRuntime;
    std::shared_ptr<Client> endpoint;
    std::thread worker;
    {
        std::lock_guard<std::mutex> guard(runtime->mutex);
        if (runtime->stopped && !runtime->worker.joinable()) return;
        runtime->stopped = true;
        runtime->connected = false;
        endpoint = runtime->client;
        worker = std::move(runtime->worker);
    }
    runtime->wake.notify_all();
    if (endpoint) endpoint->stop();
    if (worker.joinable()) worker.join();
    {
        std::lock_guard<std::mutex> guard(runtime->mutex);
        runtime->client.reset();
        runtime->configuration.bearer.clear();
    }
}

bool MaiSessionSyncSocket::isConnected() const {
    std::lock_guard<std::mutex> guard(mRuntime->mutex);
    return mRuntime->connected;
}
