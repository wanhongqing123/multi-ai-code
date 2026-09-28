#pragma once

#include <QDialog>
#include <QList>
#include <QPoint>
#include <QRect>
#include <QSize>
#include <QString>

class QFrame;
class QLabel;
class QTextBrowser;
class QTreeWidget;
class MarkdownView;

// Markdown / HTML 附件的预览窗。
//
// 存在的理由与 AppMessageDialog / AppTextInputDialog 相同：带系统标题栏的裸 QDialog
// 会挂上「?」帮助按钮、把文件名在标题栏和面板里重复显示两遍，摆在这套界面里格格不入。
// 这里沿用同一套无边框圆角面板。
//
// 与那两个小弹窗不同的是：文档预览窗口大、停留久，去掉系统标题栏后必须自己补回
// 「拖动」和「缩放」，否则窗口被钉死在屏幕中央，比原生样式更难用。
//
// 缩放要四条边都能拖，不能只给右下角一个 grip：无边框窗口没有系统缩放边框，
// 而那个 grip 又几乎看不见，用户会以为这个窗口根本不能调大小。
class FilePreviewDialog final : public QDialog {
    Q_OBJECT

public:
    enum class ContentFormat { Html, Markdown };
    // Git Diff 报告同时服务浏览器与 Qt 富文本。Qt 不支持媒体查询、CSS 自定义属性，
    // 也会忽略部分现代布局属性；在进入 QTextDocument 前集中做一次兼容化，避免调用方
    // 各自维护容易分家的替换规则。
    static QString normalizeGitDiffHtmlForQt(QString html);

    // Diff 报告顶部那份「变更文件」索引里的一行。
    struct FileIndexEntry {
        QString label;       // 仓库内相对路径（多仓库时带 <子模块>/ 前缀）
        QString anchor;      // 文档内锚点 id，形如 f0
        int additions = 0;
        int deletions = 0;
    };

    // 从报告 HTML 里读出文件索引。不是 Diff 报告、或索引不存在时返回空列表。
    static QList<FileIndexEntry> parseFileIndex(const QString& html);

    // Markdown 保留原文交给共用 MarkdownView；HTML/Diff 保留富文本预览。
    FilePreviewDialog(const QString& displayName, const QString& content, QWidget* parent = nullptr,
                      ContentFormat format = ContentFormat::Html);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    void buildUi(const QString& displayName, const QString& content, ContentFormat format);
    void applyStyle();
    void updateElidedTitle();
    QSize contentAwareInitialSize();
    // pos 为对话框坐标。返回空表示这个位置不在缩放判定带上。
    Qt::Edges resizeEdgesAt(const QPoint& pos) const;
    bool beginResize(const QPoint& globalPos, const QPoint& localPos);
    void updateResize(const QPoint& globalPos);
    void applyResizeCursor(const QPoint& localPos);
    bool handlePanelMouseEvent(QEvent* event);
    // 多文件 Diff 才建左栏：只有一个文件时列表是纯占地方。
    QWidget* buildFileListPane(const QList<FileIndexEntry>& entries, QWidget* parent);

    QFrame* panel_ = nullptr;
    QWidget* header_ = nullptr;
    QLabel* title_ = nullptr;
    QTextBrowser* content_ = nullptr;
    MarkdownView* markdownView_ = nullptr;
    QTreeWidget* fileList_ = nullptr;
    QString fullTitle_;
    QString markdownSource_;
    QPoint dragOffset_;
    bool dragging_ = false;
    Qt::Edges resizeEdges_;
    QRect resizeStartGeometry_;
    QPoint resizeStartGlobal_;
};
