#include "app/add_material_dialog.h"
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>

AddMaterialDialog::AddMaterialDialog(QWidget* parent)
    : QDialog(parent) {
    setWindowTitle("添加素材");
    setModal(true);
    resize(300, 220);

    auto* layout = new QGridLayout(this);
    layout->setSpacing(12);

    QPushButton* btnFull = new QPushButton("共享屏幕", this);
    QPushButton* btnCamera = new QPushButton("摄像头", this);
    QPushButton* btnVideo = new QPushButton("插播视频", this);

    btnFull->setFixedSize(120, 80);
    btnCamera->setFixedSize(120, 80);
    btnVideo->setFixedSize(120, 80);

    // 2x2布局：共享屏幕、摄像头在第一行，插播视频在第二行
    layout->addWidget(btnFull, 0, 0);
    layout->addWidget(btnCamera, 0, 1);
    layout->addWidget(btnVideo, 1, 0);

    connect(btnCamera, &QPushButton::clicked, this, [this]() {
        this->selected_ = AddMaterialDialog::Selection::Camera;
        accept();
    });
    connect(btnVideo, &QPushButton::clicked, this, [this]() {
        this->selected_ = AddMaterialDialog::Selection::Video;
        accept();
    });
    connect(btnFull, &QPushButton::clicked, this, [this]() {
        this->selected_ = AddMaterialDialog::Selection::Screen;
        accept();
    });
}

