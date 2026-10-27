#include "MaiCloudDriveClient.h"

#include <curl/curl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr uint64_t kPartBytes = 8 * 1024 * 1024;

void require(bool value, const char* message) {
    if (!value) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

bool exists(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    return input.good();
}

std::string readFile(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

struct Call {
    MaiCloudDriveStatus status{};
    long httpStatus{};
    std::string body;
};

Call take(MaiCloudDriveResult* raw) {
    Call result{raw->status, raw->http_status,
                raw->response_json_utf8
                    ? std::string(raw->response_json_utf8, raw->response_bytes)
                    : std::string()};
    mai_cloud_drive_result_free(raw);
    return result;
}

Call request(const MaiCloudDriveOptions& options, MaiCloudDriveAction action,
             const char* payload = nullptr) {
    MaiCloudDriveResult raw{};
    mai_cloud_drive_request(&options, action, payload, nullptr, &raw);
    return take(&raw);
}

Call upload(const MaiCloudDriveOptions& options, const std::string& path,
            const std::string& checkpoint, MaiCloudDriveProgress progress = nullptr,
            void* context = nullptr) {
    MaiCloudDriveResult raw{};
    mai_cloud_drive_upload_resumable(&options, path.c_str(), checkpoint.c_str(),
                                     "Holiday.MOV", "video/quicktime", nullptr,
                                     progress, context, &raw);
    return take(&raw);
}

int cancelOnSecondPart(uint64_t uploaded, uint64_t, void*) {
    return uploaded > kPartBytes ? 1 : 0;
}

int reportProgress(uint64_t uploaded, uint64_t total, void* opaque) {
    auto* values = static_cast<std::vector<uint64_t>*>(opaque);
    require(uploaded <= total, "upload progress exceeded file length");
    values->push_back(uploaded);
    return 0;
}

size_t collect(char* bytes, size_t size, size_t count, void* context) {
    static_cast<std::string*>(context)->append(bytes, size * count);
    return size * count;
}

std::string getState(const std::string& base, const std::string& caPath,
                     const char* route = "/test/state") {
    CURL* curl = curl_easy_init();
    require(curl != nullptr, "could not allocate control request");
    std::string response;
    const std::string url = base + route;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_CAINFO, caPath.c_str());
    curl_easy_setopt(curl, CURLOPT_PROXY, "");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, collect);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    const CURLcode code = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    require(code == CURLE_OK, "control request failed");
    return response;
}

void replaceByte(const std::string& path, char value) {
    FILE* file = std::fopen(path.c_str(), "r+b");
    require(file != nullptr, "could not change fixture file");
    require(std::fseek(file, 111, SEEK_SET) == 0 &&
            std::fwrite(&value, 1, 1, file) == 1, "could not write fixture byte");
    std::fclose(file);
}

int readMemoryAt(void* context, uint64_t offset,
                 unsigned char* output, size_t requested) {
    const auto* content = static_cast<const std::string*>(context);
    if (offset > content->size() || requested > content->size() - offset) return 0;
    std::memcpy(output, content->data() + offset, requested);
    return 1;
}

int nonSeekableRead(void*, uint64_t, unsigned char*, size_t) {
    return -1;
}

struct SequentialSource {
    const std::string* contents = nullptr;
    size_t offset = 0;
};

int readSequential(void* context, unsigned char* output,
                   size_t capacity, size_t* readBytes) {
    auto* source = static_cast<SequentialSource*>(context);
    *readBytes = std::min(capacity, source->contents->size() - source->offset);
    if (*readBytes)
        std::memcpy(output, source->contents->data() + source->offset, *readBytes);
    source->offset += *readBytes;
    return 1;
}

int cancelStage(uint64_t bytes, uint64_t, void*) {
    return bytes > 1024 * 1024 ? 1 : 0;
}

Call uploadSource(const MaiCloudDriveOptions& options,
                  const MaiCloudDriveSource& source,
                  const std::string& checkpoint,
                  MaiCloudDriveProgress progress = nullptr,
                  void* context = nullptr,
                  const MaiCloudDriveCancel* cancel = nullptr) {
    MaiCloudDriveResult raw{};
    mai_cloud_drive_upload_resumable_source(&options, &source,
        checkpoint.c_str(), "Holiday.MOV", "video/quicktime", cancel,
        progress, context, &raw);
    return take(&raw);
}

unsigned completeCalls(const std::string& state) {
    constexpr char key[] = "\"complete_calls\":";
    const size_t marker = state.find(key);
    require(marker != std::string::npos, "fixture state omitted completion count");
    return unsigned(std::strtoul(state.c_str() + marker + sizeof(key) - 1,
                                 nullptr, 10));
}

}  // namespace

int main(int argc, char** argv) {
    require(argc == 5, "expected endpoint, certificate, unrelated certificate, directory");
    MaiCloudDriveMediaInfo media{};
    require(mai_cloud_drive_media_info("Holiday.MOV", &media) == 1 &&
            std::strcmp(media.content_type_utf8, "video/quicktime") == 0 &&
            std::strcmp(media.media_type_utf8, "video") == 0 &&
            media.max_size_bytes == 2'000'000'000,
            "shared video classifier changed");
    for (const auto& fixture : {
             std::pair<const char*, const char*>{"a.jpg", "image/jpeg"},
             {"a.JPEG", "image/jpeg"}, {"a.png", "image/png"},
             {"a.WEBP", "image/webp"}, {"a.heic", "image/heic"}}) {
        require(mai_cloud_drive_media_info(fixture.first, &media) == 1 &&
                std::strcmp(media.content_type_utf8, fixture.second) == 0 &&
                std::strcmp(media.media_type_utf8, "image") == 0 &&
                media.max_size_bytes == 20'000'000,
                "shared image classifier returned wrong MIME or cap");
    }
    for (const char* invalid : {".png", "../a.png", "dir/a.jpg", "a.gif", " a.png"})
        require(mai_cloud_drive_media_info(invalid, &media) == 0,
                "shared image classifier accepted an invalid name");
    for (const auto& fixture : {
             std::pair<const char*, const char*>{"voice.m4a", "audio/mp4"},
             {"sample.gif", "image/gif"},
             {"report.pdf", "application/octet-stream"}}) {
        require(mai_session_attachment_media_info(fixture.first, &media) == 1 &&
                std::strcmp(media.content_type_utf8, fixture.second) == 0,
                "Session attachment classifier returned wrong MIME");
    }
    require(mai_session_attachment_media_info("../report.pdf", &media) == 0 &&
            mai_session_attachment_media_info("CON.pdf", &media) == 0 &&
            mai_session_attachment_media_info("bad?.jpg", &media) == 0 &&
            mai_cloud_drive_media_info("report.pdf", &media) == 0,
            "Session files leaked into personal cloud drive formats");
    const std::string endpoint = argv[1];
    const std::string ca = readFile(argv[2]);
    const std::string unrelatedCa = readFile(argv[3]);
    const std::string filePath = std::string(argv[4]) + "/holiday.mov";
    const std::string checkpoint = std::string(argv[4]) + "/holiday.maicloud.json";
    std::string contents(size_t(kPartBytes * 2 + 4096), 'v');
    {
        std::ofstream file(filePath, std::ios::binary);
        file.write(contents.data(), std::streamsize(contents.size()));
    }
    require(readFile(filePath) == contents, "test video was not created");

    const std::string authBase = endpoint + "/auth";
    MaiCloudDriveOptions author{};
    author.auth_base_url_utf8 = authBase.c_str();
    author.account_token_utf8 = "author-token";
    author.ca_pem = ca.data();
    author.ca_pem_length = ca.size();
    author.proxy_url_utf8 = "";
    author.signed_upload_host_suffix_utf8 = "localhost";
    author.connect_timeout_ms = 2000;
    author.transfer_timeout_ms = 5000;
    MaiCloudDriveOptions viewer = author;
    viewer.account_token_utf8 = "viewer-token";

    require(request(viewer, MAI_CLOUD_DRIVE_LIST_VIDEOS).body.find("\"videos\":[]") !=
            std::string::npos, "other account saw an unuploaded video");
    require(request(author, MAI_CLOUD_DRIVE_LIST_VIDEOS,
                    R"({"before":"bad cursor!"})").status ==
                    MAI_CLOUD_DRIVE_INVALID_ARGUMENT,
            "unsafe cursor was sent to account service");
    MaiCloudDriveOptions insecure = author;
    insecure.auth_base_url_utf8 = "http://localhost:1234";
    require(request(insecure, MAI_CLOUD_DRIVE_LIST_VIDEOS).status ==
            MAI_CLOUD_DRIVE_INVALID_ARGUMENT,
            "insecure account URL was accepted");
    MaiCloudDriveOptions wrongCa = author;
    wrongCa.ca_pem = unrelatedCa.data();
    wrongCa.ca_pem_length = unrelatedCa.size();
    require(request(wrongCa, MAI_CLOUD_DRIVE_LIST_VIDEOS).status ==
            MAI_CLOUD_DRIVE_NETWORK_ERROR,
            "TLS request accepted unrelated certificate");

    const Call canceled = upload(author, filePath, checkpoint, cancelOnSecondPart);
    require(canceled.status == MAI_CLOUD_DRIVE_CANCELED,
            "second-part cancellation did not stop upload");
    require(exists(checkpoint), "cancellation discarded resumable checkpoint");
    MaiCloudDriveResult localTasks{};
    mai_cloud_drive_list_local_checkpoints(argv[4], &localTasks);
    const Call taskList = take(&localTasks);
    require(taskList.status == MAI_CLOUD_DRIVE_OK &&
            taskList.body.find("Holiday.MOV") != std::string::npos &&
            taskList.body.find(filePath) != std::string::npos &&
            taskList.body.find("author-token") == std::string::npos &&
            taskList.body.find("upload_url") == std::string::npos,
            "restart task list lost source or exposed a credential");
    const std::string afterCancel = getState(endpoint, argv[2]);
    require(afterCancel.find("\"sessions\":1") != std::string::npos &&
            afterCancel.find("\"parts\":[1]") != std::string::npos,
            "first OSS part was not retained for resume");

    replaceByte(filePath, 'x');
    require(upload(author, filePath, checkpoint).status ==
            MAI_CLOUD_DRIVE_CHECKPOINT_MISMATCH,
            "changed file reused an unrelated multipart session");
    replaceByte(filePath, 'v');
    std::vector<uint64_t> progress;
    MaiCloudDriveOptions completionDefaults = author;
    completionDefaults.transfer_timeout_ms = 0;
    const Call completed = upload(completionDefaults, filePath, checkpoint,
                                  reportProgress, &progress);
    require(completed.status == MAI_CLOUD_DRIVE_OK && completed.httpStatus == 200 &&
            completed.body.find("\"status\":\"ready\"") != std::string::npos,
            "resume did not finish the video");
    require(!progress.empty() && progress.front() == kPartBytes &&
            progress.back() == contents.size(),
            "resumed progress did not include completed first part");
    require(!exists(checkpoint), "successful completion retained checkpoint");
    mai_cloud_drive_list_local_checkpoints(argv[4], &localTasks);
    require(take(&localTasks).body.find("\"uploads\":[]") != std::string::npos,
            "finished upload remained in restart task list");
    require(readFile(filePath) == contents, "upload modified the local source");
    const std::string finalState = getState(endpoint, argv[2]);
    require(finalState.find("\"sessions\":1") != std::string::npos &&
            finalState.find("\"oss_authorization\":0") != std::string::npos &&
            finalState.find("\"parts\":[1,2,3]") != std::string::npos,
            "resume created extra session, leaked token, or omitted a part");

    const std::string id = "123e4567-e89b-42d3-a456-426614174000";
    const std::string target = "{\"video_id\":\"" + id + "\"}";
    require(request(author, MAI_CLOUD_DRIVE_LIST_VIDEOS).body.find("Holiday.MOV") !=
            std::string::npos, "owner list omitted uploaded MOV");
    require(request(viewer, MAI_CLOUD_DRIVE_LIST_VIDEOS).body.find(id) ==
            std::string::npos, "private video appeared in another account");
    require(request(viewer, MAI_CLOUD_DRIVE_PLAY_LINK, target.c_str()).httpStatus == 404,
            "viewer obtained another account's play URL");
    require(request(author, MAI_CLOUD_DRIVE_PLAY_LINK, target.c_str()).body.find(
            "https://localhost:") != std::string::npos,
            "owner did not receive short-lived HTTPS play URL");
    require(request(viewer, MAI_CLOUD_DRIVE_DELETE_VIDEO, target.c_str()).httpStatus == 404,
            "viewer deleted another account's video");
    require(request(author, MAI_CLOUD_DRIVE_DELETE_VIDEO, target.c_str()).status ==
            MAI_CLOUD_DRIVE_OK, "owner could not delete private video");
    require(request(author, MAI_CLOUD_DRIVE_LIST_VIDEOS).body.find(id) ==
            std::string::npos, "deleted video remained in owner list");

    const std::string tooLarge = std::string(argv[4]) + "/too-large.mov";
    {
        FILE* sparse = std::fopen(tooLarge.c_str(), "wb");
        require(sparse != nullptr && std::fseek(sparse, 2'000'000'000, SEEK_SET) == 0 &&
                std::fputc('x', sparse) != EOF, "could not make sparse size fixture");
        std::fclose(sparse);
    }
    MaiCloudDriveResult oversized{};
    mai_cloud_drive_upload_resumable(&author, tooLarge.c_str(), checkpoint.c_str(),
        "too-large.mov", "video/quicktime", nullptr, nullptr, nullptr, &oversized);
    require(oversized.status == MAI_CLOUD_DRIVE_FILE_ERROR,
            "client accepted video larger than 2 GB");
    mai_cloud_drive_result_free(&oversized);

    const std::string uriCheckpoint = std::string(argv[4]) + "/uri.maicloud.json";
    MaiCloudDriveSource nonSeekable{
        "content://test.provider/Holiday.MOV", uint64_t(contents.size()),
        nonSeekableRead, nullptr};
    require(uploadSource(author, nonSeekable, uriCheckpoint).status ==
            MAI_CLOUD_DRIVE_SOURCE_NOT_SEEKABLE && !exists(uriCheckpoint),
            "non-seekable source started a cloud session instead of requesting staging");
    MaiCloudDriveSource uriSource{
        "content://test.provider/Holiday.MOV", uint64_t(contents.size()),
        readMemoryAt, &contents};
    require(uploadSource(author, uriSource, uriCheckpoint, cancelOnSecondPart).status ==
            MAI_CLOUD_DRIVE_CANCELED && exists(uriCheckpoint),
            "direct random-access source did not retain checkpoint on pause");
    mai_cloud_drive_list_local_checkpoints(argv[4], &localTasks);
    require(take(&localTasks).body.find("content://test.provider/Holiday.MOV") !=
            std::string::npos, "restart task lost durable provider source ID");
    MaiCloudDriveOptions retained = author;
    retained.retain_completed_checkpoint = 1;
    retained.transfer_timeout_ms = 1000;
    require(uploadSource(retained, uriSource, uriCheckpoint).status ==
            MAI_CLOUD_DRIVE_NETWORK_ERROR && exists(uriCheckpoint),
            "explicit one-second completion timeout did not retain checkpoint");
    retained.transfer_timeout_ms = 5000;
    require(uploadSource(retained, uriSource, uriCheckpoint).status ==
            MAI_CLOUD_DRIVE_OK && exists(uriCheckpoint) &&
            readFile(uriCheckpoint).find("\"completed\":true") != std::string::npos,
            "host-owned completion did not persist its checkpoint receipt");
    const std::string afterRetained = getState(endpoint, argv[2]);
    // Simulate a kill after the account service committed the video but before
    // the local completion bit was persisted. The same checkpoint must settle
    // that upload, never reserve a second video.
    std::string interruptedReceipt = readFile(uriCheckpoint);
    const std::string marker = "\"completed\":true";
    const size_t markerAt = interruptedReceipt.find(marker);
    require(markerAt != std::string::npos, "missing durable completion marker");
    interruptedReceipt.replace(markerAt, marker.size(), "\"completed\":false");
    {
        std::ofstream interrupted(uriCheckpoint, std::ios::binary | std::ios::trunc);
        interrupted << interruptedReceipt;
    }
    require(uploadSource(retained, uriSource, uriCheckpoint).status ==
            MAI_CLOUD_DRIVE_OK &&
            readFile(uriCheckpoint).find(marker) != std::string::npos &&
            getState(endpoint, argv[2]) == afterRetained,
            "restart from completed checkpoint created a duplicate session");
    require(std::remove(uriCheckpoint.c_str()) == 0,
            "host could not acknowledge its durable completion receipt");

    const std::string staged = std::string(argv[4]) + "/staged.mov";
    SequentialSource stream{&contents, 0};
    MaiCloudDriveResult stageResult{};
    mai_cloud_drive_stage_stream(staged.c_str(), contents.size(), readSequential,
        &stream, nullptr, cancelStage, nullptr, &stageResult);
    require(stageResult.status == MAI_CLOUD_DRIVE_CANCELED && !exists(staged) &&
            !exists(staged + ".maicloud-part"),
            "canceled stream staging left partial media on disk");
    mai_cloud_drive_result_free(&stageResult);
    stream.offset = 0;
    mai_cloud_drive_stage_stream(staged.c_str(), contents.size(), readSequential,
        &stream, nullptr, nullptr, nullptr, &stageResult);
    require(stageResult.status == MAI_CLOUD_DRIVE_OK &&
            readFile(staged) == contents,
            "non-seekable fallback did not atomically stage exact media bytes");
    mai_cloud_drive_result_free(&stageResult);

    const std::string cancelCompleteCheckpoint =
        std::string(argv[4]) + "/cancel-complete.maicloud.json";
    const unsigned priorCompletes = completeCalls(
        getState(endpoint, argv[2], "/test/complete-calls"));
    MaiCloudDriveCancel* completeCancel = mai_cloud_drive_cancel_create();
    require(completeCancel != nullptr, "could not allocate completion cancel token");
    std::atomic_bool sawComplete{false};
    std::thread cancelWhenCompleting([&] {
        for (unsigned attempt = 0; attempt < 200; ++attempt) {
            if (completeCalls(getState(endpoint, argv[2],
                                       "/test/complete-calls")) > priorCompletes) {
                sawComplete.store(true);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        mai_cloud_drive_cancel_request(completeCancel);
    });
    const Call canceledComplete = uploadSource(author, uriSource,
        cancelCompleteCheckpoint, nullptr, nullptr, completeCancel);
    cancelWhenCompleting.join();
    mai_cloud_drive_cancel_destroy(completeCancel);
    require(sawComplete.load() &&
            canceledComplete.status == MAI_CLOUD_DRIVE_CANCELED &&
            exists(cancelCompleteCheckpoint),
            "cancel during completion did not keep a resumable checkpoint");
    require(uploadSource(author, uriSource, cancelCompleteCheckpoint).status ==
            MAI_CLOUD_DRIVE_OK && !exists(cancelCompleteCheckpoint),
            "canceled completion could not settle on restart");

    require(upload(author, filePath, checkpoint, cancelOnSecondPart).status ==
            MAI_CLOUD_DRIVE_CANCELED && exists(checkpoint),
            "second unfinished session did not retain checkpoint");
    MaiCloudDriveResult aborted{};
    mai_cloud_drive_abort_resumable(&author, checkpoint.c_str(), nullptr, &aborted);
    require(aborted.status == MAI_CLOUD_DRIVE_OK &&
            !exists(checkpoint) &&
            std::string(aborted.response_json_utf8).find("\"status\":\"deleted\"") !=
                std::string::npos,
            "explicit discard did not remove both session and local checkpoint");
    mai_cloud_drive_result_free(&aborted);

    const std::string imagePath = std::string(argv[4]) + "/cover.png";
    const std::string imageCheckpoint =
        std::string(argv[4]) + "/cover.maicloud.json";
    {
        std::ofstream image(imagePath, std::ios::binary);
        image << "image bytes for resumable transport";
    }
    MaiCloudDriveResult imageResult{};
    mai_cloud_drive_upload_resumable(&author, imagePath.c_str(),
        imageCheckpoint.c_str(), "Cover.PNG", "image/png", nullptr,
        nullptr, nullptr, &imageResult);
    const Call uploadedImage = take(&imageResult);
    require(uploadedImage.status == MAI_CLOUD_DRIVE_OK &&
            uploadedImage.body.find("\"media_type\":\"image\"") !=
                std::string::npos && !exists(imageCheckpoint),
            "image did not use the shared resumable upload path");
    require(request(author, MAI_CLOUD_DRIVE_LIST_MEDIA).body.find(
            "\"media_type\":\"image\"") != std::string::npos,
            "owner image was missing from media list");
    require(request(viewer, MAI_CLOUD_DRIVE_THUMBNAIL_LINK,
                    target.c_str()).httpStatus == 404,
            "other account obtained a private thumbnail URL");
    const Call thumbnail = request(author, MAI_CLOUD_DRIVE_THUMBNAIL_LINK,
                                   target.c_str());
    require(thumbnail.status == MAI_CLOUD_DRIVE_OK &&
            thumbnail.body.find("x-oss-process=image/resize") !=
                std::string::npos,
            "owner did not receive a short-lived thumbnail URL");
    require(request(author, MAI_CLOUD_DRIVE_DELETE_MEDIA,
                    target.c_str()).status == MAI_CLOUD_DRIVE_OK,
            "owner could not delete private image");

    const std::string oversizedImage =
        std::string(argv[4]) + "/too-large.png";
    {
        FILE* sparse = std::fopen(oversizedImage.c_str(), "wb");
        require(sparse != nullptr && std::fseek(sparse, 20'000'000, SEEK_SET) == 0 &&
                std::fputc('x', sparse) != EOF,
                "could not make oversized image fixture");
        std::fclose(sparse);
    }
    mai_cloud_drive_upload_resumable(&author, oversizedImage.c_str(),
        imageCheckpoint.c_str(), "too-large.png", "image/png", nullptr,
        nullptr, nullptr, &imageResult);
    require(imageResult.status == MAI_CLOUD_DRIVE_FILE_ERROR &&
            !exists(imageCheckpoint), "oversized image reached the upload service");
    mai_cloud_drive_result_free(&imageResult);

    const std::string documentPath = std::string(argv[4]) + "/notes.pdf";
    const std::string documentCheckpoint = std::string(argv[4]) +
        "/notes.maicloud.json";
    {
        std::ofstream document(documentPath, std::ios::binary);
        document << "Session attachment transport fixture";
    }
    MaiCloudDriveOptions session = author;
    session.owner_device_slot_utf8 = "mac";
    session.owner_login_session_id_utf8 = "login_a";
    session.session_id_utf8 = "session_a";
    MaiCloudDriveResult documentResult{};
    mai_cloud_drive_upload_resumable(&session, documentPath.c_str(),
        documentCheckpoint.c_str(), "notes.pdf", "application/octet-stream",
        nullptr, nullptr, nullptr, &documentResult);
    const Call uploadedDocument = take(&documentResult);
    require(uploadedDocument.status == MAI_CLOUD_DRIVE_OK &&
            uploadedDocument.body.find("\"room_key\":\"mac:session_a\"") !=
                std::string::npos && !exists(documentCheckpoint),
            "Session file did not upload through its separate route");
    const Call documentLink = request(session, MAI_CLOUD_DRIVE_PLAY_LINK,
                                      target.c_str());
    require(documentLink.status == MAI_CLOUD_DRIVE_OK &&
            documentLink.body.find("mac:session_a") != std::string::npos,
            "Session file could not refresh its signed read link");
    require(request(session, MAI_SESSION_ATTACHMENT_CLEANUP_ROOM).status ==
            MAI_CLOUD_DRIVE_OK,
            "Session room cleanup did not reach the separate API");

    std::cout << "cloud drive local TLS media upload/cancel/resume/privacy passed\n";
    return 0;
}
