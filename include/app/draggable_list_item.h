#pragma once

#include <QWidget>
#include <QMouseEvent>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDragLeaveEvent>
#include <QDropEvent>

namespace live_assistant {

class DraggableListItem : public QWidget {
    Q_OBJECT

public:
    explicit DraggableListItem(QWidget *parent = nullptr);
    ~DraggableListItem() override;

    void set_index(int index);
    int get_index() const;

    void set_highlighted(bool highlighted);
    bool is_highlighted() const;

signals:
    void item_dragged(int from_index, int to_index);

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dragLeaveEvent(QDragLeaveEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

private:
    int index_;
    bool is_dragging_;
    bool is_highlighted_;
    QPoint drag_start_position_;
};

} // namespace live_assistant