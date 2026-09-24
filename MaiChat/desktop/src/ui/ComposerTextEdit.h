#pragma once

#include <QTextEdit>
#include <functional>

class QMimeData;
class QResizeEvent;
class QWidget;

// IM 与 AI 助手共用的输入框外壳。它只统一拖放、粘贴和角落按钮的位置；
// 附件如何入库、如何发送仍由各自页面的业务层决定。
class ComposerTextEdit : public QTextEdit {
public:
    explicit ComposerTextEdit(QWidget* parent = nullptr);

    // 返回 true 表示附件已被业务层消费，不再执行 QTextEdit 的默认插入。
    void setMimeHandler(std::function<bool(const QMimeData*)> handler);
    void setCornerAction(QWidget* action);
    void positionCornerAction();

protected:
    void resizeEvent(QResizeEvent* event) override;
    bool canInsertFromMimeData(const QMimeData* source) const override;
    void insertFromMimeData(const QMimeData* source) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;

private:
    static bool hasLocalFile(const QMimeData* source);

    QWidget* cornerAction_ = nullptr;
    std::function<bool(const QMimeData*)> mimeHandler_;
};
