#pragma once

#include "auth/DesktopVideoCircleAccess.h"

#include <QDialog>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <functional>
#include <memory>

class QCloseEvent;
class QLabel;
class QPushButton;
class QProgressBar;
class QScrollArea;
class QTemporaryDir;
class QVBoxLayout;
struct MaiCloudDriveCancel;
struct MaiDownloadCancel;
struct MaiCloudVod;
struct MaiCloudVodCancel;

// The desktop host owns only presentation and local paths. HTTP, multipart
// upload, checkpoint verification, and download live in shared C++ libraries.
class CloudDriveDialog final : public QDialog {
public:
    using AccessProvider = std::function<DesktopVideoCircleAccess()>;
    using AccountIdProvider = std::function<QString()>;
    using PlayCallback = std::function<void(std::shared_ptr<MaiCloudVod>, const QString&)>;
    using RelocateSelector = std::function<QString(const QString&)>;

    CloudDriveDialog(AccessProvider access, AccountIdProvider accountId,
                     PlayCallback play, QWidget* parent = nullptr,
                     RelocateSelector relocate = {});
    ~CloudDriveDialog() override;

    // 1: account cloud API returned 200; 0: feature is disabled; -1: retryable.
    static int availability(const DesktopVideoCircleAccess& access);
    void openDrive(const QString& suggestedVideo = {});
    void cancelPending();

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    struct Reply;
    struct ImageReply;
    void refresh(bool append = false);
    void refreshCheckpoints();
    void renderVideos(const QJsonArray& videos, bool append);
    void renderCheckpoints(const QJsonArray& uploads);
    void loadThumbnail(const QString& mediaId, QLabel* target);
    void resetThumbnails();
    void startUpload(const QString& sourcePath, const QString& checkpointPath = {},
                     const QString& originalName = {});
    void startPlayback(const QString& videoId, const QString& name);
    void saveVideo(const QString& videoId, const QString& name);
    void startImagePreview(const QString& mediaId);
    void deleteVideo(const QString& videoId, const QString& name);
    void discardUpload(const QString& checkpointPath, const QString& name);
    void setBusy(bool busy, const QString& message = {});
    void setStatus(const QString& message, bool error = false);
    bool sameAccount() const;
    static Reply request(const DesktopVideoCircleAccess& access, int action,
                         const QJsonObject& payload,
                         const MaiCloudDriveCancel* cancel);

    AccessProvider accessProvider_;
    AccountIdProvider accountIdProvider_;
    PlayCallback play_;
    RelocateSelector relocate_;
    QString openedAccountId_;
    QString nextCursor_;
    unsigned generation_ = 0;
    unsigned thumbnailGeneration_ = 0;
    bool busy_ = false;
    QLabel* quota_ = nullptr;
    QLabel* quotaCaption_ = nullptr;
    QProgressBar* quotaBar_ = nullptr;
    QScrollArea* uploadScroll_ = nullptr;
    QLabel* status_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QPushButton* uploadButton_ = nullptr;
    QPushButton* pauseButton_ = nullptr;
    QPushButton* moreButton_ = nullptr;
    QVBoxLayout* uploadRows_ = nullptr;
    QVBoxLayout* videoRows_ = nullptr;
    std::shared_ptr<MaiCloudDriveCancel> requestCancel_;
    std::shared_ptr<MaiDownloadCancel> downloadCancel_;
    std::shared_ptr<MaiCloudVodCancel> vodCancel_;
    std::shared_ptr<MaiCloudDriveCancel> thumbnailRequestCancel_;
    std::shared_ptr<MaiDownloadCancel> thumbnailDownloadCancel_;
    std::shared_ptr<QTemporaryDir> thumbnailDirectory_;
};
