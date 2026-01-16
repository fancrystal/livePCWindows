#pragma once

#include <QWidget>
#include <QToolButton>
#include <QLabel>
#include <memory>

namespace live_assistant {

class SceneItem;

class SceneItemRow : public QWidget {
    Q_OBJECT
public:
    explicit SceneItemRow(std::shared_ptr<SceneItem> item, const QString& display_name, QWidget* parent = nullptr);

    void set_move_up_enabled(bool enabled);

signals:
    void requestDelete(std::shared_ptr<SceneItem>);
    void requestSetting(std::shared_ptr<SceneItem>);
    void requestMoveUp(std::shared_ptr<SceneItem>);
    void visibilityToggled();

private:
    std::shared_ptr<SceneItem> item_;
    QLabel* name_{};
    QToolButton *btnUp_{}, *btnEye_{}, *btnSet_{}, *btnDel_{};

    void update_eye_icon();
};

} // namespace live_assistant
