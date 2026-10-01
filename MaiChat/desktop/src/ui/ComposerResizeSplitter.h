#pragma once

#include <QSplitter>

class ComposerResizeSplitter final : public QSplitter {
  public:
    explicit ComposerResizeSplitter(QWidget *parent = nullptr);

  protected:
    QSplitterHandle *createHandle() override;
};
