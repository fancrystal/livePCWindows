#pragma once

#include <QDialog>
#include <string>
#include <vector>

namespace Ui {
class ScreenShareDialog;
}

namespace live_assistant {

class ScreenShareDialog : public QDialog {
    Q_OBJECT

public:
    explicit ScreenShareDialog(QWidget *parent = nullptr);
    ~ScreenShareDialog();

    void set_available_screens(const std::vector<std::string>& screens);
    void set_available_windows(const std::vector<std::string>& windows);
    
    void set_fps(int fps);
    void set_resolution(const QString& resolution);

    QString get_selected_target() const;
    bool is_screen_mode() const;
    int get_fps() const;
    QString get_resolution() const;
    bool capture_cursor() const;
    bool capture_border() const;

private slots:
    void on_type_changed(int index);
    void on_ok_clicked();
    void on_cancel_clicked();

private:
    Ui::ScreenShareDialog *ui;
    std::vector<std::string> screens_;
    std::vector<std::string> windows_;
};

} // namespace live_assistant
