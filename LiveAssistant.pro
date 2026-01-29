QT       += core widgets qml quick quickwidgets graphicaleffects

TARGET = LiveAssistant
TEMPLATE = app

CONFIG += c++17

# The following define makes your compiler emit warnings if you use
# any Qt feature that has been marked deprecated (the exact warnings
# depend on your compiler). Please consult the documentation of the
# deprecated API in order to know how to port your code away from it.
DEFINES += QT_DEPRECATED_WARNINGS

# You can also make your code fail to compile if it uses deprecated APIs.
# In order to do so, uncomment the following line.
# You can also select to disable deprecated APIs only up to a certain version of Qt.
#DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0

INCLUDEPATH += include

SOURCES += \
    src/app/main.cpp \
    src/app/main_window.cpp \
    src/app/exit_dialog.cpp \
    src/scene_manager/scene_manager.cpp \
    src/video_engine/video_engine.cpp \
    src/audio_engine/audio_engine.cpp \
    src/encoder/encoder.cpp \
    src/stream_pusher/stream_pusher.cpp

HEADERS += \
    include/app/main_window.h \
    include/app/exit_dialog.h \
    include/scene_manager/scene_manager.h \
    include/video_engine/video_engine.h \
    include/audio_engine/audio_engine.h \
    include/encoder/encoder.h \
    include/stream_pusher/stream_pusher.h \
    include/common/log.h \
    include/common/error.h

FORMS += \
    src/app/main_window.ui \
    src/app/exit_dialog.ui

# Add qml resources for the new QML login UI
RESOURCES += \
    resources/qml.qrc

# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target
