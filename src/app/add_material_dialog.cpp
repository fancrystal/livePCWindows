#include "app/add_material_dialog.h"
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>

AddMaterialDialog::AddMaterialDialog(QWidget* parent)
    : QDialog(parent) {
    setWindowTitle("添加素材");
    setModal(true);
    resize(520, 420);

    auto* layout = new QGridLayout(this);
    layout->setSpacing(12);

    QPushButton* btnFull = new QPushButton("共享屏幕", this);
    QPushButton* btnCamera = new QPushButton("摄像头", this);
    QPushButton* btnWhite = new QPushButton("白板", this);
    QPushButton* btnCloud = new QPushButton("云端文档", this);
    QPushButton* btnImage = new QPushButton("图片", this);
    QPushButton* btnVideo = new QPushButton("插播视频", this);
    QPushButton* btnMore = new QPushButton("更多", this);

    btnFull->setFixedSize(120, 80);
    btnCamera->setFixedSize(120, 80);
    btnWhite->setFixedSize(120, 80);
    btnVideo->setFixedSize(120, 80);
    btnCloud->setFixedSize(120, 80);
    btnImage->setFixedSize(120, 80);
    btnMore->setFixedSize(120, 80);

    layout->addWidget(btnFull, 0, 0);
    layout->addWidget(btnCamera, 0, 1);
    layout->addWidget(btnWhite, 0, 2);
    layout->addWidget(btnCloud, 0, 3);
    layout->addWidget(btnImage, 1, 0);
    layout->addWidget(btnVideo, 1, 1);
    layout->addWidget(btnMore, 1, 2);

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
    connect(btnWhite, &QPushButton::clicked, this, [this]() {
        this->selected_ = AddMaterialDialog::Selection::Whiteboard;
        accept();
    });
    connect(btnCloud, &QPushButton::clicked, this, [this]() {
        this->selected_ = AddMaterialDialog::Selection::CloudDoc;
        accept();
    });
    connect(btnImage, &QPushButton::clicked, this, [this]() {
        this->selected_ = AddMaterialDialog::Selection::Image;
        accept();
    });
    connect(btnMore, &QPushButton::clicked, this, [this]() {
        this->selected_ = AddMaterialDialog::Selection::More;
        accept();
    });
}

