#include "app/scene_item_row.h"
#include "scene_manager/scene_manager.h"
#include <QHBoxLayout>
#include <QStyle>
#include <QPixmap>
#include <QIcon>
#include <QLabel>

namespace live_assistant {

SceneItemRow::SceneItemRow(std::shared_ptr<SceneItem> item, const QString& display_name, QWidget* parent)
    : QWidget(parent), item_(std::move(item)) {
    auto* lay = new QHBoxLayout(this);
    lay->setContentsMargins(4, 0, 4, 0);
    lay->setSpacing(4);

    // 获取源类型并设置对应图标
    QString source_id = QString::fromStdString(item_->get_source_id());
    QString icon_path;
    bool is_camera = source_id.startsWith("camera_");
    bool is_screen = source_id.startsWith("capture_");

    if (is_camera) {
        icon_path = ":/images/Camera.png";
    } else if (is_screen) {
        icon_path = ":/images/screentcast.png";
    }

    // 创建带图标的名字标签
    if (!icon_path.isEmpty()) {
        // 使用水平布局显示图标和名称
        auto* name_layout = new QHBoxLayout();
        name_layout->setSpacing(4);
        name_layout->setContentsMargins(0, 0, 0, 0);

        QLabel* icon_label = new QLabel(this);
        icon_label->setPixmap(QPixmap(icon_path).scaled(16, 16, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        icon_label->setStyleSheet("min-width: 16px; max-width: 16px;");

        name_ = new QLabel(display_name, this);
        name_->setStyleSheet("min-width: 0px;");

        name_layout->addWidget(icon_label);
        name_layout->addWidget(name_);

        // 创建一个容器 widget 来承载布局
        auto* name_container = new QWidget(this);
        name_container->setLayout(name_layout);
        name_container->setStyleSheet("background: transparent;");

        // 使用 name_container 替代 name_
        lay->addWidget(name_container, 1);
    } else {
        name_ = new QLabel(display_name, this);
        lay->addWidget(name_, 1);
    }

    auto make_btn = [this](QStyle::StandardPixmap ico, const QString& tip) {
        auto* b = new QToolButton(this);
        b->setIcon(style()->standardIcon(ico));
        b->setIconSize(QSize(16, 16));
        b->setAutoRaise(true);
        b->setToolTip(tip);
        return b;
    };

    auto make_icon_btn = [this](const QString& icon_path, const QString& tip) {
        auto* b = new QToolButton(this);
        b->setIcon(QIcon(icon_path));
        b->setIconSize(QSize(16, 16));
        b->setAutoRaise(true);
        b->setToolTip(tip);
        return b;
    };

    btnUp_  = make_btn(QStyle::SP_ArrowUp,                tr("向上一层"));
    btnEye_ = make_btn(QStyle::SP_DialogYesButton,        tr("显示/隐藏"));
    btnSet_ = make_icon_btn(":/images/Setting.png",       tr("设置"));
    btnDel_ = make_icon_btn(":/images/Delete.png",        tr("删除"));

    lay->addWidget(btnUp_);
    lay->addWidget(btnEye_);
    lay->addWidget(btnSet_);
    lay->addWidget(btnDel_);

    connect(btnUp_,  &QToolButton::clicked, this, [this]{ emit requestMoveUp(item_); });
    connect(btnSet_, &QToolButton::clicked, this, [this]{ emit requestSetting(item_); });
    connect(btnDel_, &QToolButton::clicked, this, [this]{ emit requestDelete(item_); });
    connect(btnEye_, &QToolButton::clicked, this, [this]{
        item_->set_visible(!item_->is_visible());
        update_eye_icon();
        emit visibilityToggled();
    });

    update_eye_icon();
}

void SceneItemRow::set_move_up_enabled(bool enabled) {
    if (btnUp_) {
        btnUp_->setEnabled(enabled);
    }
}

void SceneItemRow::update_eye_icon() {
    btnEye_->setIcon(style()->standardIcon(item_->is_visible() ?
                                           QStyle::SP_DialogYesButton :
                                           QStyle::SP_DialogNoButton));
}

} // namespace live_assistant
