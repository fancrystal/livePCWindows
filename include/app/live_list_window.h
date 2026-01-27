#ifndef LIVE_LIST_WINDOW_H
#define LIVE_LIST_WINDOW_H

#include <QMainWindow>
#include <QJsonArray>
#include <QJsonObject>
#include "http/network_manager.h"
#include <QPoint>

namespace Ui {
class LiveListWindow;
}

namespace live_assistant {

class LiveListWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit LiveListWindow(QWidget *parent = nullptr);
    ~LiveListWindow();

    void set_user_info(const QString& user_id, const QString& token);
    void connect_to_server();

public slots:
    void on_live_item_clicked(int index);
    void on_live_list_received(const QJsonArray& live_list);

signals:
    void live_selected(const QString& live_id);

private slots:
    void on_refreshButton_clicked();
    void on_createLiveButton_clicked();
    void on_searchButton_clicked();
    void on_prevPageButton_clicked();
    void on_nextPageButton_clicked();
    void on_page_button_clicked();

private:
    void setup_live_list();
    void load_live_list();
    void add_live_item(const QJsonObject& live_info);
    void update_pagination();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    Ui::LiveListWindow *ui;
    NetworkManager* network_manager_;
    QString user_id_;
    QString token_;
    QJsonArray live_list_;
    int current_page_;
    int total_pages_;
    QString server_address_;
    // frameless drag support
    bool dragging_;
    QPoint dragStartPos_;
private slots:
    // (QML integration removed)
};

} // namespace live_assistant

#endif // LIVE_LIST_WINDOW_H