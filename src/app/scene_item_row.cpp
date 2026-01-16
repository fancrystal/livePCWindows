#include "app/scene_item_row.h"
#include "scene_manager/scene_manager.h"
#include <QHBoxLayout>
#include <QStyle>

namespace live_assistant {

SceneItemRow::SceneItemRow(std::shared_ptr<SceneItem> item, const QString& display_name, QWidget* parent)
    : QWidget(parent), item_(std::move(item)) {
    auto* lay = new QHBoxLayout(this);
    lay->setContentsMargins(4, 0, 4, 0);
    lay->setSpacing(4);

    name_ = new QLabel(display_name, this);
    lay->addWidget(name_, 1);

    auto make_btn = [this](QStyle::StandardPixmap ico, const QString& tip) {
        auto* b = new QToolButton(this);
        b->setIcon(style()->standardIcon(ico));
        b->setIconSize(QSize(16, 16));
        b->setAutoRaise(true);
        b->setToolTip(tip);
        return b;
    };

    btnUp_  = make_btn(QStyle::SP_ArrowUp,                tr("向上一层"));
    btnEye_ = make_btn(QStyle::SP_DialogYesButton,        tr("显示/隐藏"));
    btnSet_ = make_btn(QStyle::SP_FileDialogDetailedView, tr("设置"));
    btnDel_ = make_btn(QStyle::SP_TrashIcon,              tr("删除"));

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
