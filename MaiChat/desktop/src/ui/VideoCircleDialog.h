#pragma once

#include <QDialog>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <functional>
#include <memory>

#include "auth/DesktopVideoCircleAccess.h"

class QCloseEvent;
class QLabel;
class QLineEdit;
class QPushButton;
class QScrollArea;
class QStackedWidget;
class QTabWidget;
class QVBoxLayout;
struct MaiVideoCircleCancel;
struct MaiVideoCircleMarkCancel;
struct MaiDownloadCancel;

// Desktop shell for the shared C++ video-circle protocol and media preparation.
// All HTTP, OSS upload, and downloaded-file transfer remain in MaiChat/Network.
class VideoCircleDialog final : public QDialog {
public:
    using AccessProvider = std::function<DesktopVideoCircleAccess()>;
    using AccountIdProvider = std::function<QString()>;
    // The receiver owns a downloaded temporary playback file and removes it
    // after its player has fully closed.
    using PlayCallback = std::function<void(const QString&)>;

    VideoCircleDialog(AccessProvider access, AccountIdProvider accountId,
                      PlayCallback play, QWidget* parent = nullptr);
    ~VideoCircleDialog() override;

    // A 404 means the production feature flag is off. No entry or publish
    // button should be shown until this probe succeeds for the current account.
    static bool isAvailable(const DesktopVideoCircleAccess& access);

    void openBrowse();
    void openPublish(const QString& completedCardJson, const QString& sourcePath);
    void cancelPending();

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    struct Reply;
    struct PublishReply;
    void setBusy(bool busy, const QString& message = {});
    void showError(const QString& message);
    void loadTab(int index, bool append = false);
    void renderFeed(const QJsonArray& posts, bool append);
    void renderMine(const QJsonArray& posts, bool append);
    void renderBlocks(const QJsonArray& authors);
    void playPost(const QString& postId);
    void runMutation(int action, const QJsonObject& payload, const QString& success,
                     int refreshTab);
    void submitPublication();
    bool sameAccount() const;
    static Reply request(const DesktopVideoCircleAccess& access, int action,
                         const QJsonObject& payload, const MaiVideoCircleCancel* cancel);

    AccessProvider accessProvider_;
    AccountIdProvider accountIdProvider_;
    PlayCallback play_;
    QString openedAccountId_;
    QString completedCardJson_;
    QString sourcePath_;
    QString nextCursor_;
    QString mineCursor_;
    QString afterLoadNotice_;
    QStackedWidget* pages_ = nullptr;
    QTabWidget* tabs_ = nullptr;
    QVBoxLayout* feedRows_ = nullptr;
    QVBoxLayout* mineRows_ = nullptr;
    QVBoxLayout* blockedRows_ = nullptr;
    QLineEdit* title_ = nullptr;
    QLabel* sourcePreview_ = nullptr;
    QLabel* authorLabel_ = nullptr;
    QLabel* status_ = nullptr;
    QPushButton* publishButton_ = nullptr;
    QPushButton* moreButton_ = nullptr;
    QPushButton* mineMoreButton_ = nullptr;
    bool busy_ = false;
    unsigned generation_ = 0;
    std::shared_ptr<MaiVideoCircleCancel> requestCancel_;
    std::shared_ptr<MaiVideoCircleMarkCancel> markCancel_;
    std::shared_ptr<MaiDownloadCancel> downloadCancel_;
};
