#include "app/draggable_list_item.h"
#include <QApplication>
#include <QDrag>
#include <QMimeData>
#include <QPainter>

namespace live_assistant {

DraggableListItem::DraggableListItem(QWidget *parent)
    : QWidget(parent),
      index_(-1),
      is_dragging_(false),
      is_highlighted_(false) {
    setAcceptDrops(true);
    setMinimumHeight(36);
    setMouseTracking(true); // 启用鼠标跟踪
    setAttribute(Qt::WA_TransparentForMouseEvents, false); // 确保接收鼠标事件
    setStyleSheet(
        "background-color: #333333; "
        "border-radius: 4px; "
        "margin: 2px 0; "
        "border: 1px solid transparent;"
    );
}

DraggableListItem::~DraggableListItem() {
}

void DraggableListItem::set_index(int index) {
    index_ = index;
}

int DraggableListItem::get_index() const {
    return index_;
}

void DraggableListItem::set_highlighted(bool highlighted) {
    // 避免在拖拽过程中重复设置相同的值
    if (is_highlighted_ == highlighted) {
        return;
    }
    
    is_highlighted_ = highlighted;
    
    // 确保控件有效且未被销毁时才调用update()
    if (this && !thread() && isVisible() && !isHidden() && !isWindowModified()) {
        QMetaObject::invokeMethod(this, "update", Qt::QueuedConnection);
    }
}

bool DraggableListItem::is_highlighted() const {
    return is_highlighted_;
}

void DraggableListItem::mousePressEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton) {
        drag_start_position_ = event->pos();
        event->accept(); // 接受事件，防止被其他控件拦截
    } else {
        QWidget::mousePressEvent(event);
    }
}

void DraggableListItem::mouseMoveEvent(QMouseEvent *event) {
    if (!(event->buttons() & Qt::LeftButton)) {
        return;
    }

    if ((event->pos() - drag_start_position_).manhattanLength() < QApplication::startDragDistance()) {
        return;
    }

    is_dragging_ = true;
    
    // 创建拖拽对象
    QDrag *drag = new QDrag(this);
    QMimeData *mime_data = new QMimeData;
    mime_data->setData("application/x-draggable-item", QByteArray::number(index_));
    drag->setMimeData(mime_data);
    
    // 使用Qt::MoveAction作为默认动作，避免不必要的复制
    Qt::DropAction drop_action = drag->exec(Qt::MoveAction);
    
    is_dragging_ = false;
    set_highlighted(false);
    event->accept();
}

void DraggableListItem::dragEnterEvent(QDragEnterEvent *event) {
    if (event->mimeData()->hasFormat("application/x-draggable-item")) {
        event->acceptProposedAction();
        set_highlighted(true);
    }
}

void DraggableListItem::dragMoveEvent(QDragMoveEvent *event) {
    if (event->mimeData()->hasFormat("application/x-draggable-item")) {
        event->acceptProposedAction();
    }
}

void DraggableListItem::dragLeaveEvent(QDragLeaveEvent *event) {
    set_highlighted(false);
    event->accept();
}

void DraggableListItem::dropEvent(QDropEvent *event) {
    if (event->mimeData()->hasFormat("application/x-draggable-item")) {
        int from_index = event->mimeData()->data("application/x-draggable-item").toInt();
        int to_index = index_;
        
        if (from_index != to_index) {
            emit item_dragged(from_index, to_index);
        }
        
        set_highlighted(false);
        event->acceptProposedAction();
    }
}

void DraggableListItem::paintEvent(QPaintEvent *event) {
    QWidget::paintEvent(event);
    
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    
    if (is_highlighted_) {
        // 绘制高亮背景
        QBrush highlight_brush(QColor(0, 120, 215, 50));
        painter.setBrush(highlight_brush);
        painter.setPen(Qt::NoPen);
        
        // 绘制圆角矩形
        painter.drawRoundedRect(rect().adjusted(1, 1, -1, -1), 3, 3);
        
        // 绘制边框
        QPen border_pen(QColor(0, 120, 215, 150));
        border_pen.setWidth(1);
        painter.setPen(border_pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(rect().adjusted(0, 0, -1, -1), 3, 3);
    } else {
        // 绘制正常状态的边框
        QPen border_pen(QColor(50, 50, 50));
        border_pen.setWidth(1);
        painter.setPen(border_pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(rect().adjusted(0, 0, -1, -1), 3, 3);
    }
}

} // namespace live_assistant