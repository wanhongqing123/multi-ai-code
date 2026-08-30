#pragma once

#include <QDialog>
#include <QPoint>
#include <QString>

class QLabel;
class QTextBrowser;

// Markdown / HTML 附件的预览窗。
//
// 存在的理由与 AppMessageDialog / AppTextInputDialog 相同：带系统标题栏的裸 QDialog
// 会挂上「?」帮助按钮、把文件名在标题栏和面板里重复显示两遍，摆在这套界面里格格不入。
// 这里沿用同一套无边框圆角面板。
//
// 与那两个小弹窗不同的是：文档预览窗口大、停留久，去掉系统标题栏后必须自己补回
// 「拖动」和「缩放」，否则窗口被钉死在屏幕中央，比原生样式更难用。
class FilePreviewDialog final : public QDialog {
    Q_OBJECT

public:
    // Git Diff 报告同时服务浏览器与 Qt 富文本。Qt 不支持媒体查询、CSS 自定义属性，
    // 也会忽略部分现代布局属性；在进入 QTextDocument 前集中做一次兼容化，避免调用方
    // 各自维护容易分家的替换规则。
    static QString normalizeGitDiffHtmlForQt(QString html);

    // html 由调用方渲染好（markdown 转换 / 原始 HTML 各走各的），
    // 这样本类不碰文件读取与 markdown 细节，纯粹负责外观与交互。
    FilePreviewDialog(const QString& displayName, const QString& html, QWidget* parent = nullptr);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void buildUi(const QString& displayName, const QString& html);
    void applyStyle();
    void updateElidedTitle();

    QWidget* header_ = nullptr;
    QLabel* title_ = nullptr;
    QTextBrowser* content_ = nullptr;
    QString fullTitle_;
    QPoint dragOffset_;
    bool dragging_ = false;
};
