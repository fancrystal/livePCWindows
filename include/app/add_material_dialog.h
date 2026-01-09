#pragma once
#include <QDialog>

class AddMaterialDialog : public QDialog {
public:
    enum class Selection {
        None,
        Camera,
        Screen,
        Video,
        Image,
        Whiteboard,
        CloudDoc,
        More
    };

    explicit AddMaterialDialog(QWidget* parent = nullptr);
    Selection selected() const { return selected_; }

private:
    Selection selected_ = Selection::None;
    // added Video option
};

