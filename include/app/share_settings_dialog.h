#pragma once

#include <QDialog>
#include <QCheckBox>
#include <QLabel>

namespace live_assistant {

class ShareSettingsDialog : public QDialog {
    Q_OBJECT

public:
    explicit ShareSettingsDialog(QWidget* parent = nullptr);
    ~ShareSettingsDialog();

    // 设置初始值
    void setInitialValues(bool capture_cursor, bool capture_border);

    // 获取设置值
    bool isCaptureCursor() const { return capture_cursor_; }
    bool isCaptureBorder() const { return capture_border_; }

protected:
    void showEvent(QShowEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private:
    void setupUI();
    void applyStyle();
    bool checkWin11Support();

    // UI 控件
    QCheckBox* check_cursor_ = nullptr;
    QCheckBox* check_border_ = nullptr;
    QLabel* warning_label_ = nullptr;

    // 设置值
    bool capture_cursor_ = true;
    bool capture_border_ = true;
};

} // namespace live_assistant
