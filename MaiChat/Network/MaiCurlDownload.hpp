#pragma once

#include "MaiCurlDownload.h"

#include <memory>

namespace mai_chat {

class DownloadCancellation final {
public:
    DownloadCancellation()
        : token_(mai_download_cancel_create(), mai_download_cancel_destroy) {}

    void request() const { mai_download_cancel_request(token_.get()); }
    const MaiDownloadCancel* get() const { return token_.get(); }
    bool valid() const { return token_ != nullptr; }

private:
    std::unique_ptr<MaiDownloadCancel, decltype(&mai_download_cancel_destroy)> token_;
};

inline MaiDownloadResult downloadFile(const MaiDownloadOptions& options,
                                      const DownloadCancellation* cancellation = nullptr) {
    MaiDownloadResult result{};
    mai_download_file(&options, cancellation ? cancellation->get() : nullptr, &result);
    return result;
}

}  // namespace mai_chat
