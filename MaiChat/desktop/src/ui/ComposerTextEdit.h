// 文件作用：桌面端共用界面；ComposerTextEdit 负责富文本输入、选区与编辑操作，本文件给出其声明。
// 类型关系：ComposerTextEdit : public QTextEdit；审阅时可据此追踪父类、接口或回调契约。
// 对外边界：本文件给出 ComposerTextEdit 的类型和函数声明；参数、返回值和对象生命周期以此为准。
// 公开入口：hasLocalFile。
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
    void setLeadingAction(QWidget* action);
    void setSecondaryLeadingAction(QWidget* action);
    void setLeadingHint(QWidget* hint);
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
    QWidget* leadingAction_ = nullptr;
    QWidget* secondaryLeadingAction_ = nullptr;
    QWidget* leadingHint_ = nullptr;
    std::function<bool(const QMimeData*)> mimeHandler_;
};
