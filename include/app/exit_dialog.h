#pragma once

#include <QDialog>

namespace Ui {
class ExitDialog;
}

namespace live_assistant {

class ExitDialog : public QDialog {
    Q_OBJECT

public:
    explicit ExitDialog(QWidget *parent = nullptr);
    ~ExitDialog();

    // 获取用户选择的操作类型
    enum class Action {
        Minimize,  // 最小化到托盘
        Exit       // 退出程序
    };

    Action getSelectedAction() const { return selected_action_; }

    // 检查是否记住选择
    bool shouldRememberChoice() const { return remember_choice_; }

    // 设置窗口图标（可选）
    void setWindowIcon(const QIcon &icon);

protected:
    void showEvent(QShowEvent *event) override;

private:
    Ui::ExitDialog *ui;
    Action selected_action_ = Action::Minimize;  // 默认选择最小化
    bool remember_choice_ = false;
    
    void setupConnections();
    void applyStyle();
};

} // namespace live_assistant
