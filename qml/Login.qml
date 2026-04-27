import QtQuick 2.15
import QtQuick.Layouts 1.15
import QtQuick.Controls 2.15

Rectangle {
    id: root
    width: 1280
    height: 760
    color: "transparent"

    // 登录状态属性
    property string loginStatus: ""
    property string loginError: ""
    property bool isLoggingIn: false
    property bool localStreamExpanded: false

    Image {
        id: bg
        anchors.fill: parent
        source: "qrc:/images/Frame back.png"
        fillMode: Image.PreserveAspectCrop
    }

    // 关闭按钮
    Rectangle {
        id: closeBtn
        width: 36
        height: 36
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.margins: 12
        color: "transparent"
        radius: 6

        // X图标
        Text {
            anchors.centerIn: parent
            text: "×"
            color: "white"
            font.pixelSize: 24
            font.weight: Font.Light
        }

        MouseArea {
            anchors.fill: parent
            hoverEnabled: true
            onEntered: {
                parent.color = Qt.rgba(1, 0, 0, 0.3)
            }
            onExited: {
                parent.color = "transparent"
            }
            onClicked: {
                if (typeof loginWindow !== 'undefined') {
                    loginWindow.close()
                } else {
                    Qt.quit()
                }
            }
        }
    }

    Rectangle {
        id: panel
        width: 380
        height: 500
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.verticalCenter: parent.verticalCenter
        radius: 14
        color: "transparent"

        Rectangle {
            id: card
            anchors.centerIn: parent
            width: parent.width
            height: parent.height
            radius: 24
            clip: true
            color: Qt.rgba(0.06,0.02,0.04,0.86)
            border.width: 1
            border.color: Qt.rgba(1,1,1,0.03)
        }

        ColumnLayout {
            id: cardLayout
            anchors.fill: card
            anchors.margins: 26
            spacing: 12

            Text {
                text: "灵犀 · 视频云"
                color: "white"
                font.pixelSize: 22
                horizontalAlignment: Text.AlignHCenter
                Layout.alignment: Qt.AlignHCenter
            }

            // 状态提示文字
            Text {
                id: statusText
                text: ""
                color: "transparent"
                font.pixelSize: 12
                horizontalAlignment: Text.AlignHCenter
                Layout.alignment: Qt.AlignHCenter
                Layout.preferredWidth: parent.width
                Layout.preferredHeight: 16  // 固定高度，即使不可见也占据空间
                wrapMode: Text.WordWrap
                opacity: text !== "" ? 1.0 : 0.0  // 使用opacity而不是visible，避免布局跳动
            }

            // 监听状态变化来更新statusText
            Binding {
                target: statusText
                property: "text"
                value: loginStatus !== "" ? loginStatus : loginError
                when: loginStatus !== "" || loginError !== ""
            }

            Binding {
                target: statusText
                property: "color"
                value: isLoggingIn ? "#4aa6ff" : "#ff6b6b"
                when: isLoggingIn || loginError !== ""
            }

            Text { text: "用户名"; color: Qt.rgba(1,1,1,0.6); font.pixelSize: 12 }

            Rectangle {
                width: parent.width
                height: 40
                radius: 6
                color: "white"
                border.width: 1
                border.color: Qt.rgba(0,0,0,0.06)

                RowLayout {
                    anchors.fill: parent
                    anchors.margins: 8
                    spacing: 8
                    Image {
                        source: "qrc:/images/Frame_user.png"
                        width: 20
                        height: 20
                        opacity: 0.6
                        Layout.alignment: Qt.AlignVCenter
                    }
                    TextField {
                        id: accountField
                        placeholderText: "请输入用户名"
                        color: "#222222"
                        background: Rectangle { color: "transparent"; border.width: 0 }
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                    }
                }
            }

            Text { text: "密码"; color: Qt.rgba(1,1,1,0.6); font.pixelSize: 12 }

            Rectangle {
                width: parent.width
                height: 40
                radius: 6
                color: "white"
                border.width: 1
                border.color: Qt.rgba(0,0,0,0.06)

                RowLayout {
                    anchors.fill: parent
                    anchors.margins: 8
                    spacing: 8
                    Image {
                        source: "qrc:/images/Frame_pass.png"
                        width: 20
                        height: 20
                        opacity: 0.6
                        Layout.alignment: Qt.AlignVCenter
                    }
                    TextField {
                        id: passwordField
                        echoMode: TextInput.Password
                        placeholderText: "请输入密码"
                        color: "#222222"
                        background: Rectangle { color: "transparent"; border.width: 0 }
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                    }
                    Rectangle {
                        width: 28
                        height: 28
                        radius: 6
                        color: "white"
                        Layout.alignment: Qt.AlignVCenter

                        Image {
                            anchors.centerIn: parent
                            source: "qrc:/images/close.png"
                            width: 14
                            height: 14
                            opacity: 0.8
                        }
                        MouseArea {
                            anchors.fill: parent
                            onClicked: passwordField.echoMode = passwordField.echoMode === TextInput.Password ? TextInput.Normal : TextInput.Password
                        }
                    }
                }
            }

            RowLayout {
                spacing: 8
                CheckBox { id: rememberBox; checked: true }
                Text { text: "记住密码"; color: Qt.rgba(1,1,1,0.6); font.pixelSize: 12 }
            }

            Button {
                id: loginBtn
                height: 42
                Layout.preferredWidth: parent.width * 0.9
                Layout.alignment: Qt.AlignHCenter

                background: Rectangle {
                    radius: 6
                    gradient: Gradient {
                        GradientStop { position: 0; color: "#4a6ef0" }
                        GradientStop { position: 1; color: "#f05a6a" }
                    }
                }

                contentItem: Text {
                    id: loginText
                    text: isLoggingIn ? "登录中..." : "登录"
                    color: "white"
                    font.pixelSize: 14
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }

                onClicked: {
                    handleLogin()
                }
            }

            Text {
                text: "—— 登录即同意《用户协议》与《隐私政策》 ——"
                font.pixelSize: 10
                color: Qt.rgba(1,1,1,0.45)
                horizontalAlignment: Text.AlignHCenter
                Layout.alignment: Qt.AlignHCenter
            }

            // 本地推流按钮（点击后右侧滑出面板）
            Button {
                id: localStreamToggleBtn
                height: 32
                Layout.preferredWidth: parent.width * 0.9
                Layout.alignment: Qt.AlignHCenter
                Layout.topMargin: 8

                background: Rectangle {
                    radius: 4
                    color: Qt.rgba(1,1,1,0.08)
                    border.width: 1
                    border.color: Qt.rgba(1,1,1,0.15)
                }

                contentItem: RowLayout {
                    anchors.centerIn: parent
                    spacing: 6
                    Text {
                        text: "本地推流"
                        color: Qt.rgba(1,1,1,0.7)
                        font.pixelSize: 12
                    }
                    Text {
                        text: localStreamExpanded ? "◀" : "▶"
                        color: Qt.rgba(1,1,1,0.5)
                        font.pixelSize: 10
                    }
                }

                onClicked: {
                    localStreamExpanded = !localStreamExpanded
                }
            }

            // 清理缓存按钮
            Button {
                id: clearCacheBtn
                height: 32
                Layout.preferredWidth: parent.width * 0.9
                Layout.alignment: Qt.AlignHCenter
                Layout.topMargin: 4

                background: Rectangle {
                    radius: 4
                    color: Qt.rgba(1,1,1,0.08)
                    border.width: 1
                    border.color: Qt.rgba(1,1,1,0.15)
                }

                contentItem: Text {
                    text: "清理缓存"
                    color: Qt.rgba(1,1,1,0.7)
                    font.pixelSize: 12
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }

                onClicked: {
                    if (typeof loginWindow !== 'undefined' && loginWindow.qmlClearCache) {
                        var result = loginWindow.qmlClearCache()
                        cacheResultPopup.message = result
                        cacheResultPopup.visible = true
                    }
                }
            }
        }
    }

    // 右侧滑出的本地推流面板（独立于卡片，向右展开）
    Rectangle {
        id: localStreamSidePanel
        anchors.left: panel.right
        anchors.leftMargin: 8
        anchors.verticalCenter: panel.verticalCenter
        width: localStreamExpanded ? 320 : 0
        height: 340
        clip: true
        radius: 16
        color: Qt.rgba(0.06,0.02,0.04,0.92)
        border.width: 1
        border.color: Qt.rgba(1,1,1,0.08)

        Behavior on width {
            NumberAnimation { duration: 220; easing.type: Easing.OutCubic }
        }

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 20
            spacing: 12
            visible: localStreamExpanded

            Text {
                text: "本地推流"
                color: "white"
                font.pixelSize: 16
                font.weight: Font.Medium
                Layout.alignment: Qt.AlignHCenter
            }

            Text {
                text: "推流地址"
                color: Qt.rgba(1,1,1,0.6)
                font.pixelSize: 12
            }

            Rectangle {
                Layout.fillWidth: true
                height: 40
                radius: 6
                color: "white"
                border.width: 1
                border.color: Qt.rgba(0,0,0,0.06)

                RowLayout {
                    anchors.fill: parent
                    anchors.margins: 8
                    spacing: 8
                    TextField {
                        id: rtmpUrlField
                        placeholderText: "rtmp://localhost:1935/live/stream_key"
                        color: "#222222"
                        background: Rectangle { color: "transparent"; border.width: 0 }
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                    }
                }
            }

            Button {
                id: localStreamBtn
                height: 42
                Layout.fillWidth: true
                Layout.topMargin: 8

                background: Rectangle {
                    radius: 6
                    color: "#2d8c5f"
                }

                contentItem: Text {
                    text: "开始推流"
                    color: "white"
                    font.pixelSize: 14
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }

                onClicked: handleLocalStream()
            }

            Text {
                text: "跳过直播列表，直接进入推流\n（不支持插播视频）"
                font.pixelSize: 11
                color: Qt.rgba(1,1,1,0.45)
                horizontalAlignment: Text.AlignHCenter
                Layout.alignment: Qt.AlignHCenter
                wrapMode: Text.WordWrap
            }
        }
    }

    // 登录处理函数 - 在QML端做初步验证
    function handleLogin() {
        if (isLoggingIn) return

        var phone = accountField.text.trim()
        var password = passwordField.text

        // 清空之前的状态
        loginStatus = ""
        loginError = ""
        statusText.text = ""

        if (phone === "") {
            loginError = "请输入用户名"
            statusText.text = loginError
            statusText.color = "#ff6b6b"
            return
        }
        if (password === "") {
            loginError = "密码不能为空"
            statusText.text = loginError
            statusText.color = "#ff6b6b"
            return
        }

        // QML验证通过，设置为登录中状态
        isLoggingIn = true
        statusText.text = "正在登录..."
        statusText.color = "#4aa6ff"

        console.log("QML validation passed, calling C++ login...")

        // 调用C++登录
        if (typeof loginWindow !== 'undefined' && loginWindow.qmlLogin) {
            loginWindow.qmlLogin(phone, password, rememberBox.checked)
        }
    }

    // 本地推流处理函数
    function handleLocalStream() {
        if (isLoggingIn) return

        var rtmpUrl = rtmpUrlField.text.trim()

        // 清空之前的状态
        loginStatus = ""
        loginError = ""
        statusText.text = ""

        // 验证推流地址
        if (rtmpUrl === "") {
            loginError = "请输入推流地址"
            statusText.text = loginError
            statusText.color = "#ff6b6b"
            return
        }

        // 验证 RTMP 地址格式
        if (!rtmpUrl.startsWith("rtmp://") && !rtmpUrl.startsWith("rtmps://")) {
            loginError = "推流地址必须以 rtmp:// 或 rtmps:// 开头"
            statusText.text = loginError
            statusText.color = "#ff6b6b"
            return
        }

        // 设置为登录中状态
        isLoggingIn = true
        statusText.text = "正在启动本地推流..."
        statusText.color = "#4aa6ff"

        console.log("Starting local stream with URL: " + rtmpUrl)

        // 调用C++启动本地推流
        if (typeof loginWindow !== 'undefined' && loginWindow.qmlStartLocalStream) {
            loginWindow.qmlStartLocalStream(rtmpUrl)
        }
    }

    // 组件加载时读取保存的凭据
    Component.onCompleted: {
        console.log("Login.qml component completed")
        if (typeof loginWindow !== 'undefined') {
            console.log("loginWindow is defined")
            // 检查是否有保存的凭据
            if (loginWindow.qmlHasSavedCredentials()) {
                var savedUsername = loginWindow.qmlGetSavedUsername()
                console.log("Saved username: " + savedUsername)
                if (savedUsername !== "") {
                    accountField.text = savedUsername
                    passwordField.text = loginWindow.qmlGetSavedPassword()
                }
            }
        } else {
            console.log("loginWindow is NOT defined")
        }
    }

    // 监听登录状态变化 - 使用直接方法调用
    Connections {
        target: loginWindow

        function onLogin_failed(errorMsg) {
            console.log("QML onLogin_failed: " + errorMsg)
            handleLoginFailed(errorMsg)
        }

        function onLogin_success() {
            console.log("QML onLogin_success called")
            isLoggingIn = false
            loginStatus = "登录成功！"
            statusText.text = "登录成功！"
            statusText.color = "#4aa6ff"
        }

        function onLocal_stream_success(rtmpUrl) {
            console.log("QML onLocal_stream_success: " + rtmpUrl)
            isLoggingIn = false
            loginStatus = "正在进入推流..."
            statusText.text = "正在进入推流..."
            statusText.color = "#4aa6ff"
        }

    }

    // 清理缓存结果弹窗
    Rectangle {
        id: cacheResultPopup
        property string message: ""

        visible: false
        anchors.centerIn: parent
        width: 280
        height: 120
        radius: 12
        color: Qt.rgba(0.06, 0.02, 0.04, 0.96)
        border.width: 1
        border.color: Qt.rgba(1, 1, 1, 0.15)
        z: 100

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 20
            spacing: 16

            Text {
                text: cacheResultPopup.message
                color: "white"
                font.pixelSize: 13
                Layout.alignment: Qt.AlignHCenter
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                Layout.fillWidth: true
            }

            Button {
                Layout.alignment: Qt.AlignHCenter
                Layout.preferredWidth: 80
                height: 30

                background: Rectangle {
                    radius: 4
                    color: Qt.rgba(1, 1, 1, 0.12)
                    border.width: 1
                    border.color: Qt.rgba(1, 1, 1, 0.2)
                }

                contentItem: Text {
                    text: "确定"
                    color: "white"
                    font.pixelSize: 13
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }

                onClicked: cacheResultPopup.visible = false
            }
        }
    }

    // 统一的登录失败处理
    function handleLoginFailed(errorMsg) {
        isLoggingIn = false
        loginError = errorMsg
        loginStatus = ""
        statusText.text = errorMsg
        statusText.color = "#ff6b6b"
        console.log("handleLoginFailed: isLoggingIn=" + isLoggingIn + ", error=" + errorMsg)
    }
}
