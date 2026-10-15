// 文件作用：桌面端共用界面；ComposerResizeSplitter 负责输入区与消息区的可调整分隔布局，本文件给出其声明。
// 类型关系：ComposerResizeSplitter final : public QSplitter；审阅时可据此追踪父类、接口或回调契约。
// 对外边界：本文件给出 ComposerResizeSplitter 的类型和函数声明；参数、返回值和对象生命周期以此为准。
#pragma once

#include <QSplitter>

class ComposerResizeSplitter final : public QSplitter {
  public:
    explicit ComposerResizeSplitter(QWidget *parent = nullptr);

  protected:
    QSplitterHandle *createHandle() override;
};
