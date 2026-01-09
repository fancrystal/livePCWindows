#pragma once
#include <QDialog>
#include <QPushButton>

class AddMaterialDialog : public QDialog {
    Q_OBJECT
public:
    explicit AddMaterialDialog(QWidget* parent = nullptr);

signals:
    void chooseCamera();
    void chooseScreen();
    void chooseImage();
    void chooseWhiteboard();
    void chooseCloudDoc();
    void chooseMore();
};

