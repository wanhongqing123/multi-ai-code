#include "MaiSessionSyncSocket.h"

#include <MaiThread.h>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

#include <curl/curl.h>
#include <websocketpp/client.hpp>
#include <websocketpp/config/asio_client.hpp>

namespace {

// 仅替换 TCP 目的地址；WebSocket URI、Host、TLS SNI 与证书校验仍使用原域名。
template <typename Config>
class RoutedTransport : public websocketpp::transport::asio::endpoint<Config> {
    using Base = websocketpp::transport::asio::endpoint<Config>;
public:
    void setDialAddress(std::string address) { address_ = std::move(address); }
protected:
    void async_connect(typename Base::transport_con_ptr connection, websocketpp::uri_ptr uri,
                       websocketpp::transport::connect_handler callback) {
        if (address_.empty()) {
            Base::async_connect(connection, uri, callback);
            return;
        }
        const std::string host = address_.find(':') == std::string::npos ? address_ : "[" + address_ + "]";
        auto dial = websocketpp::lib::make_shared<websocketpp::uri>(
            "wss://" + host + ":" + uri->get_port_str() + "/");
        Base::async_connect(connection, dial, callback);
        // 基类同步保存 URI 后才异步解析；恢复原域名供后续 TLS 初始化使用。
        connection->set_uri(uri);
    }
private:
    std::string address_;
};

struct RoutedConfig : websocketpp::config::asio_tls_client {
    using transport_type = RoutedTransport<transport_config>;
};
using Client = websocketpp::client<RoutedConfig>;

size_t discardProbe(char*, size_t size, size_t count, void*) { return size * count; }

// 不带凭据的 HTTPS 探测既验证 DoH 返回的地址，也不重放任何账号写操作。
std::string probeAddress(const std::string& caPem, const char* doh) {
#ifdef MAICHAT_SESSION_SYNC_TEST
    // 本地 fixture 注入已验证目的地址；正式构建绝不读取测试变量。
    if (std::string(doh) == "https://1.1.1.1/dns-query") {
        if (const char* address = std::getenv("MAICHAT_TEST_WSS_SECOND_ADDRESS")) return address;
    }
    if (const char* address = std::getenv("MAICHAT_TEST_WSS_ADDRESS")) return address;
#endif
    // 此模块可在账号或模型网络组件启动之前独立运行；不依赖它们初始化 curl。
    static std::once_flag curlInitialization;
    static CURLcode curlInitializationResult = CURLE_FAILED_INIT;
    std::call_once(curlInitialization, [] {
        curlInitializationResult = curl_global_init(CURL_GLOBAL_DEFAULT);
    });
    if (curlInitializationResult != CURLE_OK) return {};
    CURL* curl = curl_easy_init();
    if (!curl) return {};
    curl_blob ca{const_cast<char*>(caPem.data()), caPem.size(), CURL_BLOB_COPY};
    CURLcode code = CURLE_OK;
    auto set = [&](CURLoption option, auto value) {
        if (code == CURLE_OK) code = curl_easy_setopt(curl, option, value);
    };
    set(CURLOPT_URL, "https://ichat.life/api/v1/auth/me");
    set(CURLOPT_PROXY, "");
    set(CURLOPT_NOPROXY, "*");
    set(CURLOPT_DOH_URL, doh);
    set(CURLOPT_DOH_SSL_VERIFYPEER, 1L);
    set(CURLOPT_DOH_SSL_VERIFYHOST, 2L);
    set(CURLOPT_CAINFO_BLOB, &ca);
    set(CURLOPT_SSL_VERIFYPEER, 1L);
    set(CURLOPT_SSL_VERIFYHOST, 2L);
    set(CURLOPT_FOLLOWLOCATION, 0L);
    set(CURLOPT_NOSIGNAL, 1L);
    set(CURLOPT_CONNECTTIMEOUT_MS, 3500L);
    set(CURLOPT_TIMEOUT_MS, 4000L);
    set(CURLOPT_WRITEFUNCTION, discardProbe);
    if (code == CURLE_OK) code = curl_easy_perform(curl);
    long status = 0;
    char* address = nullptr;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_getinfo(curl, CURLINFO_PRIMARY_IP, &address);
    std::string result = code == CURLE_OK && status == 401 && address ? address : "";
    asio::error_code error;
    if (!result.empty()) asio::ip::make_address(result, error);
    if (error) result.clear();
    curl_easy_cleanup(curl);
    return result;
}

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
        unsigned fallback = 0;
        std::string dialAddress;
        while (true) {
            Configuration settings;
            {
                std::lock_guard<std::mutex> guard(mutex);
                if (stopped) return;
                settings = configuration;
            }
            auto endpoint = std::make_shared<Client>();
            endpoint->setDialAddress(dialAddress);
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
            bool opened = false;
            std::string failure;
            bool httpRejected = false;
            endpoint->set_open_handler([this, &opened](websocketpp::connection_hdl handle) {
                opened = true;
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
                [&failure, &httpRejected, weak = std::weak_ptr<Client>(endpoint)](websocketpp::connection_hdl handle) {
                    const auto active = weak.lock();
                    if (!active) return;
                    const auto failed = active->get_con_from_hdl(handle);
                    failure = failed->get_ec().message();
                    httpRejected = failed->get_response_code() >= 400;
                });
            websocketpp::lib::error_code error;
            auto dial = endpoint->get_connection(settings.url, error);
            if (!error) {
                dial->append_header("Authorization", "Bearer " + settings.bearer);
                if (dialAddress.empty() && !settings.proxyUrl.empty()) dial->set_proxy(settings.proxyUrl, error);
            }
            if (error) {
                failure = error.message();
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
                    failure = "WebSocket transport failed";
                }
            }
            if (opened) {
                fallback = 0;
                dialAddress.clear();
            } else if (!httpRejected && websocketpp::uri(settings.url).get_host() == "ichat.life" &&
                       (websocketpp::uri(settings.url).get_port() == 443
#ifdef MAICHAT_SESSION_SYNC_TEST
                        || std::getenv("MAICHAT_TEST_WSS_ADDRESS") != nullptr
#endif
                       ) && fallback < 2) {
                // 只在账号域名的系统线路失败后尝试可信探测，不改变其他服务的代理策略。
                // 上一个备用地址握手失败后必须丢弃它，才能继续探测下一条线路。
                dialAddress.clear();
                while (fallback < 2 && dialAddress.empty()) {
                    {
                        std::lock_guard<std::mutex> guard(mutex);
                        if (stopped) return;
                    }
                    const char* doh = fallback++ == 0 ? "https://223.5.5.5/dns-query" :
                                                       "https://1.1.1.1/dns-query";
                    dialAddress = probeAddress(settings.caPem, doh);
                }
                if (!dialAddress.empty()) continue;
            }
            if (!opened && !failure.empty()) notify(Event::Error, failure);
            if (!opened && fallback >= 2) {
                fallback = 0;
                dialAddress.clear();
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
