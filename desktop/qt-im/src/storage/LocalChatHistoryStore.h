#pragma once

#include <QString>

#include "model/ChatState.h"

class LocalChatHistoryStore {
public:
    explicit LocalChatHistoryStore(QString rootDir);

    bool save(const ChatState& state) const;
    bool load(const QString& ownerUserId, ChatState& state) const;

private:
    QString filePath(const QString& ownerUserId) const;

    QString rootDir_;
};
