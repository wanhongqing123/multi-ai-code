#pragma once

#include <QLabel>
#include <QListWidget>
#include <QMainWindow>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QTextEdit>
#include <QVBoxLayout>

#include "app/RemoteIMApplication.h"

class QResizeEvent;
class QShowEvent;
class QLineEdit;
class QHBoxLayout;
class QListWidgetItem;
class QPoint;

class MainWindow final : public QMainWindow {
public:
    explicit MainWindow(RemoteIMApplication& app, QWidget* parent = nullptr);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    void buildUi();
    void applyStyle();
    void bindSignals();
    void refresh();
    void refreshContacts();
    void refreshContactDirectory();
    void refreshSettings();
    void refreshMessages();
    void applyConversationFilter();
    void showMessagesPage();
    void showContactsPage();
    void showSettingsPage();
    void syncNavigationSelection();
    void updateNavigationSelection(QPushButton* selectedButton);
    void openAddContactDialog();
    void openImagePicker();
    void openImagePreview(const QString& imagePath);
    void openFilePreview(const RemoteIMFileAttachment& attachment);
    QWidget* createMessageBubble(const RemoteIMMessage& message);
    int messageBubbleMaximumWidth() const;
    void applyMessageBubbleWidth(QWidget* bubble, bool expanded) const;
    void updateMessageBubbleWidths();
    QWidget* createSettingsRow(const QString& title, QLabel* valueLabel, const QString& helperText);
    void sendCurrentText();
    void updateComposerState();
    void updateSlashCommandSuggestions();
    void selectSlashCommand(const QString& command);
    void showContactContextMenu(QListWidget* list, const QPoint& pos);
    void deleteContactFromItem(QListWidgetItem* item);
    void deleteSelectedContactFromList(QListWidget* list);
    QString contactName(const QString& userId) const;

    RemoteIMApplication& app_;
    QWidget* navRail_ = nullptr;
    QLineEdit* navSearchInput_ = nullptr;
    QPushButton* messageNavButton_ = nullptr;
    QPushButton* contactsNavButton_ = nullptr;
    QPushButton* settingsNavButton_ = nullptr;
    QStackedWidget* contentStack_ = nullptr;
    QWidget* messagesPage_ = nullptr;
    QWidget* contactsPage_ = nullptr;
    QWidget* settingsPage_ = nullptr;
    QListWidget* conversationList_ = nullptr;
    QListWidget* contactsList_ = nullptr;
    QLabel* titleLabel_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QLabel* settingsAccountValue_ = nullptr;
    QLabel* settingsConnectionValue_ = nullptr;
    QLabel* settingsSdkAppIdValue_ = nullptr;
    QScrollArea* messageScroll_ = nullptr;
    QWidget* messageContainer_ = nullptr;
    QVBoxLayout* messageLayout_ = nullptr;
    // One-shot connection used to jump to the latest message once the message
    // list layout has settled (bubble heights depend on width / word wrap, so
    // the scrollbar range is only correct after a later layout pass).
    QMetaObject::Connection messageScrollToBottomConn_;
    QPushButton* addContactButton_ = nullptr;
    QPushButton* voiceButton_ = nullptr;
    QPushButton* imageButton_ = nullptr;
    QPushButton* sendButton_ = nullptr;
    QTextEdit* messageEditor_ = nullptr;
    QWidget* slashCommandBar_ = nullptr;
    QHBoxLayout* slashCommandLayout_ = nullptr;
};
