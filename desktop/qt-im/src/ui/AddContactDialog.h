#pragma once

#include <QDialog>
#include <QLineEdit>
#include <QPushButton>

class AddContactDialog final : public QDialog {
    Q_OBJECT

public:
    explicit AddContactDialog(QWidget* parent = nullptr);

    QString userId() const;
    void setUserId(const QString& userId);

private:
    void buildUi();
    void applyStyle();
    void updateConfirmButton();

    QLineEdit* userIdInput_ = nullptr;
    QPushButton* confirmButton_ = nullptr;
};
