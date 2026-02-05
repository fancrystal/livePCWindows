#ifndef LIVE_LIST_WINDOW_H
#define LIVE_LIST_WINDOW_H

#include <QMainWindow>
#include <QJsonArray>
#include <QJsonObject>
#include <QComboBox>
#include <QPoint>
#include "http/live_item.h"
namespace Ui {
class LiveListWindow;
}
namespace live_assistant {
    class LiveListWindow : public QMainWindow {
        Q_OBJECT

    public:
        explicit LiveListWindow(const QString& user_id = QString(), const QString& token = QString(), QWidget* parent = nullptr);
        ~LiveListWindow();

    public slots:
        void on_live_item_clicked(int index);
        void on_live_list_received(const QJsonArray& live_list);

    signals:
        void live_selected(const QString& live_id, const LiveItem& liveItem);

    private slots:
        void on_refreshButton_clicked();
        void on_createLiveButton_clicked();
        void on_searchButton_clicked();
        void on_prevPageButton_clicked();
        void on_nextPageButton_clicked();
        void on_page_button_clicked();
        void on_categoryComboBox_currentIndexChanged(int index);

    private:
        void setup_live_list();
        void load_live_list();
        void add_live_item(const QJsonObject& live_info);
        void update_pagination();
        int getCurrentRoomState() const;
        void filter_live_list(const QString& keyword);  // 根据关键字过滤直播列表

    protected:
        bool eventFilter(QObject* watched, QEvent* event) override;

    private:
        Ui::LiveListWindow* ui;
        QString user_id_;
        QString token_;
        QList<LiveItem> full_live_items_;   // 保存完整的LiveItem列表（未过滤）
        QList<LiveItem> current_live_items_; // 当前显示的LiveItem列表（可能被搜索过滤）
        QJsonArray full_live_list_;         // 保存完整的直播列表JSON（未过滤）
        QJsonArray live_list_;              // 当前显示的直播列表（可能被搜索过滤）
        int current_page_;
        int total_pages_;
        QString server_address_;
        int current_status_index_;
        QString current_search_keyword_;  // 当前搜索关键字
        bool dragging_;
        QPoint dragStartPos_;
    };
}

#endif // LIVE_LIST_WINDOW_H
