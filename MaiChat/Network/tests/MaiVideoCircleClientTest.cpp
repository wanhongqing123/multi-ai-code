#include "MaiVideoCircleClient.h"

#include <curl/curl.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <regex>
#include <string>
#include <thread>

namespace {

struct CallResult {
    MaiVideoCircleStatus status{};
    long httpStatus{};
    std::string body;
};

void require(bool passed, const char* message) {
    if (!passed) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

std::string readFile(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

void writeFile(const std::string& path, const std::string& content) {
    std::ofstream output(path, std::ios::binary);
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    require(output.good(), "could not write fixture MP4");
}

std::string stringField(const std::string& json, const std::string& name) {
    const std::regex expression("\\\"" + name + "\\\"\\s*:\\s*\\\"([^\\\"]*)\\\"");
    std::smatch match;
    return std::regex_search(json, match, expression) ? match[1].str() : std::string();
}

CallResult request(const MaiVideoCircleOptions& options, MaiVideoCircleAction action,
                   const char* payload = nullptr) {
    MaiVideoCircleResult raw{};
    mai_video_circle_request(&options, action, payload, nullptr, &raw);
    CallResult result{raw.status, raw.http_status,
                      raw.response_json_utf8 ? std::string(raw.response_json_utf8,
                          raw.response_bytes) : std::string()};
    mai_video_circle_result_free(&raw);
    return result;
}

CallResult upload(const MaiVideoCircleOptions& options, const std::string& signedJson,
                  const std::string& path, uint64_t bytes) {
    MaiVideoCircleResult raw{};
    mai_video_circle_upload_file(&options, signedJson.c_str(), path.c_str(), bytes,
                                 nullptr, &raw);
    CallResult result{raw.status, raw.http_status,
                      raw.response_json_utf8 ? std::string(raw.response_json_utf8,
                          raw.response_bytes) : std::string()};
    mai_video_circle_result_free(&raw);
    return result;
}

size_t collect(char* data, size_t size, size_t count, void* opaque) {
    static_cast<std::string*>(opaque)->append(data, size * count);
    return size * count;
}

std::string fixtureRequest(const std::string& url, const std::string& caPath,
                           bool post = false) {
    CURL* curl = curl_easy_init();
    require(curl != nullptr, "fixture control curl allocation failed");
    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_CAINFO, caPath.c_str());
    curl_easy_setopt(curl, CURLOPT_PROXY, "");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, collect);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 5000L);
    if (post) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "");
    }
    const CURLcode code = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    require(code == CURLE_OK && status == 200, "fixture control request failed");
    return response;
}

}  // namespace

int main(int argc, char** argv) {
    require(argc == 5, "expected local TLS URL, two certificates, and temp directory");
    require(curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK,
            "curl initialization failed");
    const std::string base = argv[1];
    const std::string certificate = readFile(argv[2]);
    const std::string unrelatedCertificate = readFile(argv[3]);
    const std::string directory = argv[4];
    const std::string authBase = base + "/auth";
    const std::string mediaBase = base + "/media";
    const std::string filePath = directory + "/clip.mp4";
    const std::string fileContents = std::string("\0\0\0\x18" "ftypmp42", 12) +
                                     std::string(988, 'v');
    writeFile(filePath, fileContents);
    require(readFile(filePath) == fileContents, "fixture MP4 file changed");

    const char* completedCard = R"({"tool":"agent_send_media","state":"completed","output":{"delivery":"current_ai_session","type":"video","mime_type":"video/mp4","path":"relative.mp4","source_tool":"wan_video","source_task_id":"task-1"}})";
    MaiVideoCircleResult inspected{};
    mai_video_circle_inspect_card(completedCard, &inspected);
    require(inspected.status == MAI_VIDEO_CIRCLE_OK &&
                std::string(inspected.response_json_utf8).find("\"source_task_id\":\"task-1\"") !=
                    std::string::npos,
            "completed AI card was not recognized by shared eligibility parser");
    mai_video_circle_result_free(&inspected);
    mai_video_circle_inspect_card(
        R"({"tool":"agent_send_media","state":"completed","output":"{\"delivery\":\"current_ai_session\",\"type\":\"video\",\"mime_type\":\"video/mp4\",\"path\":\"relative.mp4\",\"source_tool\":\"wan_video\",\"source_task_id\":\"task-1\"}"})",
        &inspected);
    require(inspected.status == MAI_VIDEO_CIRCLE_OK,
            "completed card with original JSON-string output was not recognized");
    mai_video_circle_result_free(&inspected);
    mai_video_circle_inspect_card(
        R"({"tool":"agent_send_media","state":"completed","output":{"delivery":"current_ai_session","type":"video","mime_type":"video/mp4","path":"relative.mp4","source_tool":"wan_video"}})",
        &inspected);
    require(inspected.status == MAI_VIDEO_CIRCLE_INVALID_ARGUMENT,
            "card without a source task was eligible for publishing");
    mai_video_circle_result_free(&inspected);

    MaiVideoCircleOptions author{};
    author.auth_base_url_utf8 = authBase.c_str();
    author.media_base_url_utf8 = mediaBase.c_str();
    author.account_token_utf8 = "author-token";
    author.ca_pem = certificate.data();
    author.ca_pem_length = certificate.size();
    author.proxy_url_utf8 = "";
    author.signed_upload_host_suffix_utf8 = "localhost";
    author.connect_timeout_ms = 2000;
    author.transfer_timeout_ms = 5000;
    MaiVideoCircleOptions viewer = author;
    viewer.account_token_utf8 = "viewer-token";

    const CallResult emptyFeed = request(viewer, MAI_VIDEO_CIRCLE_FEED);
    require(emptyFeed.status == MAI_VIDEO_CIRCLE_OK && emptyFeed.httpStatus == 200 &&
                emptyFeed.body.find("\"posts\":[]") != std::string::npos,
            "initial viewer feed was not empty");

    const CallResult signedUpload = request(author, MAI_VIDEO_CIRCLE_SIGN_UPLOAD,
        R"({"filename":"clip.mp4","size_bytes":1000})");
    require(signedUpload.status == MAI_VIDEO_CIRCLE_OK && signedUpload.httpStatus == 200,
            "signed upload request failed");
    require(signedUpload.body.find("read_url") == std::string::npos,
            "signing leaked a public playback URL");
    const std::string key = stringField(signedUpload.body, "object_key");
    require(key.find("video-circle/author/") == 0, "signed object is not author scoped");

    const CallResult oversized = upload(author, signedUpload.body, filePath, 200000001);
    require(oversized.status == MAI_VIDEO_CIRCLE_INVALID_ARGUMENT,
            "upload accepted a source larger than the 200 MB limit");
    require(readFile(filePath) == fileContents, "oversized upload changed its source file");

    std::string mismatchedURL = signedUpload.body;
    const size_t ownerPath = mismatchedURL.find("/video-circle/author/");
    require(ownerPath != std::string::npos, "fixture signed URL has no owner path");
    mismatchedURL.replace(ownerPath, std::string("/video-circle/author/").size(),
                          "/video-circle/viewer/");
    require(upload(author, mismatchedURL, filePath, 1000).status ==
                MAI_VIDEO_CIRCLE_INVALID_ARGUMENT,
            "signed URL was not bound to its object key");

    std::string insecureURL = signedUpload.body;
    const size_t scheme = insecureURL.find("https://");
    require(scheme != std::string::npos, "fixture signed URL has no HTTPS scheme");
    insecureURL.replace(scheme, std::string("https://").size(), "http://");
    require(upload(author, insecureURL, filePath, 1000).status ==
                MAI_VIDEO_CIRCLE_INVALID_ARGUMENT,
            "plain HTTP signed upload was accepted");

    const CallResult uploaded = upload(author, signedUpload.body, filePath, 1000);
    require(uploaded.status == MAI_VIDEO_CIRCLE_OK && uploaded.httpStatus == 200,
            "signed HTTPS PUT failed");
    require(readFile(filePath) == fileContents, "upload changed its source file");
    const std::string afterUpload = fixtureRequest(base + "/test/state", argv[2]);
    require(afterUpload.find("\"signed_puts\":1") != std::string::npos &&
                afterUpload.find("\"auth_on_put\":0") != std::string::npos,
            "signed PUT was missing or carried an account credential");

    const std::string publish =
        std::string(R"({"completed_card":{"tool":"agent_send_media","state":"completed","output":{"delivery":"current_ai_session","type":"video","mime_type":"video/mp4","path":"relative.mp4","source_tool":"wan_video","source_task_id":"task-1"}},"title":"Local AI clip","object_key":")") +
        key + R"(","size_bytes":1000,"duration_ms":8000})";
    const std::string invalidCard =
        std::string(R"({"completed_card":{"tool":"agent_send_media","state":"completed","output":{"delivery":"current_ai_session","type":"video","mime_type":"video/mp4","path":"relative.mp4","source_tool":"wan_video"}},"title":"Local AI clip","object_key":")") +
        key + R"(","size_bytes":1000,"duration_ms":8000})";
    require(request(author, MAI_VIDEO_CIRCLE_PUBLISH_DRAFT, invalidCard.c_str()).status !=
                MAI_VIDEO_CIRCLE_OK, "missing source task was accepted");
    require(request(viewer, MAI_VIDEO_CIRCLE_PUBLISH_DRAFT, publish.c_str()).status !=
                MAI_VIDEO_CIRCLE_OK, "viewer published another account's upload");
    require(fixtureRequest(base + "/test/state", argv[2]).find("\"post_count\":0") !=
                std::string::npos, "invalid submissions created a post");

    const CallResult created = request(author, MAI_VIDEO_CIRCLE_PUBLISH_DRAFT,
                                       publish.c_str());
    require(created.status == MAI_VIDEO_CIRCLE_OK && created.httpStatus == 201 &&
                stringField(created.body, "status") == "pending",
            "completed AI card did not create a pending post");
    const std::string postID = stringField(created.body, "post_id");
    require(!postID.empty(), "new post ID is absent");
    require(request(viewer, MAI_VIDEO_CIRCLE_FEED).body.find(postID) == std::string::npos,
            "pending post leaked into viewer feed");
    require(request(author, MAI_VIDEO_CIRCLE_MINE).body.find("pending") !=
                std::string::npos, "author cannot see pending post");
    const CallResult minePage = request(author, MAI_VIDEO_CIRCLE_MINE,
                                        R"({"before":"mine-cursor"})");
    require(minePage.status == MAI_VIDEO_CIRCLE_OK &&
                minePage.body.find("\"cursor_received\":\"mine-cursor\"") !=
                    std::string::npos,
            "shared client did not forward the author-list cursor");
    const std::string target = std::string("{\"post_id\":\"") + postID + "\"}";
    const CallResult pendingViewerLink = request(viewer, MAI_VIDEO_CIRCLE_PLAY_LINK,
                                                  target.c_str());
    require(pendingViewerLink.status == MAI_VIDEO_CIRCLE_HTTP_ERROR &&
                pendingViewerLink.httpStatus == 404,
            "viewer opened pending video before review");
    require(request(author, MAI_VIDEO_CIRCLE_PLAY_LINK, target.c_str()).status ==
                MAI_VIDEO_CIRCLE_OK, "author pending preview was unavailable");

    fixtureRequest(base + "/test/review", argv[2], true);
    require(request(viewer, MAI_VIDEO_CIRCLE_FEED).body.find(postID) != std::string::npos,
            "approved post is absent from viewer feed");
    const CallResult playback = request(viewer, MAI_VIDEO_CIRCLE_PLAY_LINK,
                                        target.c_str());
    require(playback.status == MAI_VIDEO_CIRCLE_OK &&
                stringField(playback.body, "read_url").find(base + "/oss/") == 0,
            "viewer did not receive a short-lived playback URL");
    const std::string reportPayload = std::string("{\"post_id\":\"") + postID +
                                      "\",\"reason\":\"spam\"}";
    require(request(viewer, MAI_VIDEO_CIRCLE_REPORT, reportPayload.c_str()).status ==
                MAI_VIDEO_CIRCLE_OK, "viewer could not report approved video");
    require(request(viewer, MAI_VIDEO_CIRCLE_BLOCKS_LIST).body.find("\"authors\":[]") !=
                std::string::npos, "initial blocked-author list was not empty");
    require(request(viewer, MAI_VIDEO_CIRCLE_BLOCK,
                    R"({"author_id":"author"})").status == MAI_VIDEO_CIRCLE_OK,
            "viewer could not block published author");
    require(request(viewer, MAI_VIDEO_CIRCLE_FEED).body.find(postID) == std::string::npos,
            "blocked author's post remained in feed");
    require(request(viewer, MAI_VIDEO_CIRCLE_BLOCKS_LIST).body.find("\"author_id\":\"author\"") !=
                std::string::npos, "blocked author was absent from management list");
    require(request(author, MAI_VIDEO_CIRCLE_BLOCKS_LIST).body.find("\"authors\":[]") !=
                std::string::npos, "blocked-author list leaked across accounts");
    require(request(viewer, MAI_VIDEO_CIRCLE_UNBLOCK,
                    R"({"author_id":"author"})").status == MAI_VIDEO_CIRCLE_OK,
            "viewer could not unblock author");
    require(request(viewer, MAI_VIDEO_CIRCLE_FEED).body.find(postID) != std::string::npos,
            "unblocked author's post did not return to feed");
    const CallResult deniedWithdraw = request(viewer, MAI_VIDEO_CIRCLE_WITHDRAW,
                                              target.c_str());
    require(deniedWithdraw.status == MAI_VIDEO_CIRCLE_HTTP_ERROR &&
                deniedWithdraw.httpStatus == 404,
            "viewer withdrew another account's post");
    const CallResult withdrawn = request(author, MAI_VIDEO_CIRCLE_WITHDRAW,
                                         target.c_str());
    require(withdrawn.status == MAI_VIDEO_CIRCLE_OK && withdrawn.httpStatus == 200 &&
                stringField(withdrawn.body, "status") == "withdrawn",
            "author withdrawal failed");
    require(request(viewer, MAI_VIDEO_CIRCLE_FEED).body.find(postID) ==
                std::string::npos, "withdrawn post remained in viewer feed");
    require(request(viewer, MAI_VIDEO_CIRCLE_PLAY_LINK, target.c_str()).httpStatus == 404,
            "withdrawn post still had a playback link");

    const CallResult redirectSigned = request(author, MAI_VIDEO_CIRCLE_SIGN_UPLOAD,
        R"({"filename":"redirect.mp4","size_bytes":1000})");
    require(redirectSigned.status == MAI_VIDEO_CIRCLE_OK, "redirect fixture signing failed");
    const CallResult redirectUpload = upload(author, redirectSigned.body, filePath, 1000);
    require(redirectUpload.status != MAI_VIDEO_CIRCLE_OK,
            "signed PUT followed a redirect");
    const std::string finalState = fixtureRequest(base + "/test/state", argv[2]);
    require(finalState.find("\"redirect_followed\":0") != std::string::npos &&
                finalState.find("\"auth_on_put\":0") != std::string::npos,
            "redirect leaked a signed PUT or account credential");

    MaiVideoCircleOptions wrongTLS = author;
    wrongTLS.ca_pem = unrelatedCertificate.data();
    wrongTLS.ca_pem_length = unrelatedCertificate.size();
    require(request(wrongTLS, MAI_VIDEO_CIRCLE_FEED).status ==
                MAI_VIDEO_CIRCLE_NETWORK_ERROR,
            "server certificate signed by another CA was accepted");

    MaiVideoCircleCancel* cancel = mai_video_circle_cancel_create();
    require(cancel != nullptr, "could not allocate video-circle cancellation handle");
    MaiVideoCircleResult canceledResult{};
    std::thread slow([&] {
        mai_video_circle_request(&author, MAI_VIDEO_CIRCLE_FEED,
                                 R"({"before":"slow"})", cancel, &canceledResult);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    mai_video_circle_cancel_request(cancel);
    slow.join();
    require(canceledResult.status == MAI_VIDEO_CIRCLE_CANCELED,
            "in-flight metadata request did not cancel");
    mai_video_circle_result_free(&canceledResult);
    mai_video_circle_cancel_destroy(cancel);

    curl_global_cleanup();
    std::cout << "local TLS video-circle C ABI cases passed" << std::endl;
    return 0;
}
