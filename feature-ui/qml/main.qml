import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import QtQuick.Window 2.15
import MonitorDemo 1.0

ApplicationWindow {
    id: root

    visible: true
    visibility: Window.FullScreen
    flags: Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint
    color: "transparent"
    title: qsTr("监视器demo")

    property bool hudVisible: true
    property bool gridEnabled: false
    property bool frameGuideEnabled: false
    property bool centerMarkerEnabled: false
    property bool safeAreaEnabled: false
    property bool touchFocusMode: false
    property bool touchIndicatorVisible: false
    property real touchIndicatorX: landscapeUi.width / 2
    property real touchIndicatorY: landscapeUi.height / 2
    property string activePanel: ""
    readonly property var falseColorPalette: [
        "#5c2291", "#1f50dc", "#00b8eb", "#2abe60",
        "#808080", "#ee70a5", "#ffcf33", "#eb2d34"
    ]

    function wakeHud() {
        hudVisible = true
        hudTimer.restart()
    }

    function closePanels() {
        activePanel = ""
    }

    function togglePanel(panelName) {
        activePanel = activePanel === panelName ? "" : panelName
        wakeHud()
    }

    function optionIndex(options, value) {
        for (var index = 0; index < options.length; ++index) {
            if (Number(options[index].value) === Number(value))
                return index
        }
        return -1
    }

    Timer {
        id: hudTimer
        interval: 4000
        repeat: false
        running: true
        onTriggered: {
            if (root.activePanel.length === 0)
                root.hudVisible = false
        }
    }

    Timer {
        id: touchIndicatorTimer
        interval: 900
        repeat: false
        onTriggered: root.touchIndicatorVisible = false
    }

    Item {
        id: landscapeUi
        anchors.centerIn: parent
        width: root.height
        height: root.width
        rotation: 90

        Rectangle {
            anchors.fill: parent
            color: "#030405"
            visible: !videoController.streaming
        }

        MouseArea {
            id: hudTapArea
            anchors.fill: parent
            z: 1
            onClicked: {
                if (root.activePanel.length > 0) {
                    root.closePanels()
                    root.wakeHud()
                } else if (root.hudVisible) {
                    root.hudVisible = false
                    hudTimer.stop()
                } else {
                    root.wakeHud()
                }
            }
        }

        MouseArea {
            id: touchFocusArea
            anchors.fill: parent
            enabled: root.touchFocusMode && cameraController.connected
                     && cameraController.canTouchFocus
            z: 40
            onClicked: {
                root.touchIndicatorX = mouse.x
                root.touchIndicatorY = mouse.y
                root.touchIndicatorVisible = true
                touchIndicatorTimer.restart()

                // Rotated landscape UI and source image now share the same axes.
                cameraController.touchFocus(mouse.x / width, mouse.y / height)
                root.touchFocusMode = false
                root.wakeHud()
            }
        }

        MonitorAnalysisOverlay {
            anchors.fill: parent
            controller: videoController
            visible: videoController.streaming
                     && (videoController.zebraEnabled
                         || videoController.peakingEnabled
                         || videoController.waveformEnabled)
            z: 2
        }

        Item {
            id: guides
            anchors.fill: parent
            visible: root.gridEnabled || root.frameGuideEnabled
                     || root.centerMarkerEnabled || root.safeAreaEnabled
            z: 3

            Item {
                anchors.fill: parent
                visible: root.gridEnabled

                Repeater {
                    model: [1 / 3, 2 / 3]
                    Rectangle {
                        x: guides.width * modelData
                        width: 1
                        height: guides.height
                        color: "#8cffffff"
                    }
                }

                Repeater {
                    model: [1 / 3, 2 / 3]
                    Rectangle {
                        y: guides.height * modelData
                        width: guides.width
                        height: 1
                        color: "#8cffffff"
                    }
                }
            }

            Rectangle {
                readonly property real targetRatio: 2.39
                readonly property real availableRatio: guides.width / guides.height
                width: availableRatio > targetRatio
                       ? guides.height * targetRatio : guides.width
                height: availableRatio > targetRatio
                        ? guides.height : guides.width / targetRatio
                anchors.centerIn: parent
                color: "transparent"
                border.width: 2
                border.color: "#d9ffffff"
                visible: root.frameGuideEnabled
            }

            Rectangle {
                width: parent.width * 0.9
                height: parent.height * 0.9
                anchors.centerIn: parent
                color: "transparent"
                border.width: 1
                border.color: "#b8ffffff"
                visible: root.safeAreaEnabled
            }

            Item {
                width: 46
                height: 46
                anchors.centerIn: parent
                visible: root.centerMarkerEnabled

                Rectangle {
                    width: 46
                    height: 2
                    anchors.centerIn: parent
                    color: "#e8ffffff"
                }
                Rectangle {
                    width: 2
                    height: 46
                    anchors.centerIn: parent
                    color: "#e8ffffff"
                }
                Rectangle {
                    width: 8
                    height: 8
                    radius: 4
                    anchors.centerIn: parent
                    color: "#030405"
                    border.width: 1
                    border.color: "#ffffff"
                }
            }
        }

        Row {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 22
            spacing: 0
            visible: videoController.streaming && videoController.falseColorEnabled
            z: 9

            Repeater {
                model: root.falseColorPalette

                Rectangle {
                    width: 62
                    height: 14
                    color: modelData
                }
            }
        }

        Item {
            x: root.touchIndicatorX - width / 2
            y: root.touchIndicatorY - height / 2
            width: 52
            height: 52
            visible: root.touchIndicatorVisible
            z: 41

            Rectangle {
                anchors.fill: parent
                color: "transparent"
                border.width: 2
                border.color: "#f6f7f8"
            }
            Rectangle {
                width: 16
                height: 2
                anchors.centerIn: parent
                color: "#f6f7f8"
            }
            Rectangle {
                width: 2
                height: 16
                anchors.centerIn: parent
                color: "#f6f7f8"
            }
        }

        Rectangle {
            id: signalMessage
            width: Math.min(parent.width - 80, 620)
            height: signalColumn.implicitHeight + 42
            anchors.centerIn: parent
            radius: 4
            color: "#e6111417"
            border.width: 1
            border.color: hdmiController.state === 4 || videoController.state === 4
                          ? "#e1a143" : "#42484d"
            visible: !videoController.streaming
            z: 8

            Column {
                id: signalColumn
                width: parent.width - 42
                anchors.centerIn: parent
                spacing: 8

                Text {
                    width: parent.width
                    text: hdmiController.state === 4
                          ? qsTr("HDMI 输入异常") : hdmiController.stateText
                    color: "#f6f7f8"
                    font.pixelSize: 24
                    font.weight: Font.DemiBold
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                }

                Text {
                    width: parent.width
                    text: hdmiController.lastError.length > 0
                          ? hdmiController.lastError : videoController.lastError
                    color: "#e1a143"
                    font.pixelSize: 14
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    visible: text.length > 0
                }
            }
        }

        Row {
            id: hdmiStatus
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.leftMargin: 24
            anchors.topMargin: 20
            spacing: 9
            visible: root.hudVisible
            z: 12

            Rectangle {
                width: 10
                height: 10
                radius: 5
                anchors.verticalCenter: parent.verticalCenter
                color: hdmiController.signalPresent ? "#5de176" : "#e1a143"
            }

            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: hdmiController.signalPresent
                      ? "HDMI  " + hdmiController.width + " x "
                        + hdmiController.height + "  "
                        + hdmiController.frameRateText + "p"
                      : qsTr("HDMI 无信号")
                color: "#f6f7f8"
                font.pixelSize: 16
                font.weight: Font.DemiBold
                style: Text.Outline
                styleColor: "#b0000000"
            }
        }

        Row {
            id: cameraStatus
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.rightMargin: 24
            anchors.topMargin: 20
            spacing: 9
            visible: root.hudVisible
            z: 12

            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: cameraController.connected
                      ? cameraController.modelName : cameraController.connectionText
                color: "#f6f7f8"
                font.pixelSize: 16
                font.weight: Font.DemiBold
                style: Text.Outline
                styleColor: "#b0000000"
            }

            Rectangle {
                width: cameraController.recording ? 54 : 10
                height: cameraController.recording ? 24 : 10
                radius: cameraController.recording ? 2 : 5
                anchors.verticalCenter: parent.verticalCenter
                color: cameraController.recording ? "#d62d3b"
                                                  : cameraController.connected
                                                    ? "#5de176" : "#e1a143"

                Text {
                    anchors.centerIn: parent
                    text: qsTr("REC")
                    color: "#ffffff"
                    font.pixelSize: 13
                    font.weight: Font.Bold
                    visible: cameraController.recording
                }
            }
        }

        Rectangle {
            id: leftRail
            width: 94
            height: leftTools.implicitHeight + 18
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            color: "#d914171a"
            border.width: 1
            border.color: "#4a5157"
            visible: root.hudVisible && root.activePanel !== "display"
            z: 20

            Column {
                id: leftTools
                width: parent.width
                anchors.centerIn: parent
                spacing: 2

                MonitorButton {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: cameraController.recording ? qsTr("停止") : qsTr("录制")
                    iconSource: "qrc:/assets/icons/circle.svg"
                    active: cameraController.recording
                    accentColor: "#ef4050"
                    enabled: cameraController.connected && cameraController.canRecord
                    toolTipText: qsTr("相机录制")
                    onClicked: {
                        cameraController.setRecording(!cameraController.recording)
                        root.wakeHud()
                    }
                }

                MonitorButton {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: qsTr("AF")
                    iconSource: "qrc:/assets/icons/camera.svg"
                    enabled: cameraController.connected && cameraController.canAutoFocus
                    onPressed: cameraController.setAutoFocus(true)
                    onReleased: cameraController.setAutoFocus(false)
                    onCanceled: cameraController.setAutoFocus(false)
                }

                MonitorButton {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: qsTr("触控对焦")
                    iconSource: "qrc:/assets/icons/frame.svg"
                    active: root.touchFocusMode
                    enabled: cameraController.connected && cameraController.canTouchFocus
                    onClicked: {
                        root.closePanels()
                        root.touchFocusMode = true
                        root.hudVisible = false
                        hudTimer.stop()
                    }
                }

                MonitorButton {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: qsTr("显示设置")
                    iconSource: "qrc:/assets/icons/settings.svg"
                    active: root.activePanel === "display"
                    onClicked: root.togglePanel("display")
                }
            }
        }

        Rectangle {
            id: rightRail
            width: 94
            height: rightTools.implicitHeight + 18
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            color: "#d914171a"
            border.width: 1
            border.color: "#4a5157"
            visible: root.hudVisible
            z: 22

            Column {
                id: rightTools
                width: parent.width
                anchors.centerIn: parent
                spacing: 2

                MonitorButton {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: qsTr("相机")
                    iconSource: "qrc:/assets/icons/camera.svg"
                    active: root.activePanel === "camera"
                    accentColor: cameraController.connected ? "#5de176" : "#e1a143"
                    onClicked: root.togglePanel("camera")
                }

                MonitorButton {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: qsTr("伪色")
                    iconSource: "qrc:/assets/icons/palette.svg"
                    active: videoController.falseColorEnabled
                    accentColor: "#ffcf33"
                    onClicked: {
                        videoController.toggleFalseColor()
                        root.wakeHud()
                    }
                }

                MonitorButton {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: qsTr("斑马纹")
                    iconSource: "qrc:/assets/icons/scan-line.svg"
                    active: videoController.zebraEnabled
                    accentColor: "#ffcf33"
                    onClicked: {
                        videoController.setZebraEnabled(!videoController.zebraEnabled)
                        root.wakeHud()
                    }
                }

                MonitorButton {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: qsTr("峰值")
                    iconSource: "qrc:/assets/icons/focus.svg"
                    active: videoController.peakingEnabled
                    accentColor: "#ff3440"
                    onClicked: {
                        videoController.setPeakingEnabled(!videoController.peakingEnabled)
                        root.wakeHud()
                    }
                }

                MonitorButton {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: qsTr("波形图")
                    iconSource: "qrc:/assets/icons/chart-no-axes-combined.svg"
                    active: videoController.waveformEnabled
                    accentColor: "#5ae884"
                    onClicked: {
                        videoController.setWaveformEnabled(!videoController.waveformEnabled)
                        root.wakeHud()
                    }
                }

                MonitorButton {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: videoController.zoomFactor + "x"
                    iconSource: "qrc:/assets/icons/zoom-in.svg"
                    active: videoController.zoomFactor > 1
                    onClicked: {
                        videoController.cycleZoomFactor()
                        root.wakeHud()
                    }
                }

                MonitorButton {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: qsTr("网格")
                    iconSource: "qrc:/assets/icons/grid-3x3.svg"
                    active: root.gridEnabled
                    onClicked: {
                        root.gridEnabled = !root.gridEnabled
                        root.wakeHud()
                    }
                }

                MonitorButton {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: qsTr("画幅线")
                    iconSource: "qrc:/assets/icons/frame.svg"
                    active: root.frameGuideEnabled
                    onClicked: {
                        root.frameGuideEnabled = !root.frameGuideEnabled
                        root.wakeHud()
                    }
                }

                MonitorButton {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: videoController.fillModeText
                    iconSource: "qrc:/assets/icons/maximize.svg"
                    active: videoController.fillMode === 1
                    onClicked: {
                        videoController.toggleFillMode()
                        root.wakeHud()
                    }
                }

                MonitorButton {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: qsTr("设置")
                    iconSource: "qrc:/assets/icons/settings.svg"
                    active: root.activePanel === "display"
                    onClicked: root.togglePanel("display")
                }
            }
        }

        Rectangle {
            id: displayPanel
            width: 560
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            color: "#ed111417"
            border.width: 1
            border.color: "#4a5157"
            visible: root.hudVisible && root.activePanel === "display"
            z: 30

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 30
                spacing: 18

                RowLayout {
                    Layout.fillWidth: true

                    Button {
                        id: closeDisplayButton
                        width: 46
                        height: 46
                        text: "×"
                        onClicked: root.closePanels()
                        background: Rectangle { color: "transparent" }
                        contentItem: Text {
                            text: closeDisplayButton.text
                            color: "#f6f7f8"
                            font.pixelSize: 34
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                    }

                    Text {
                        text: qsTr("监视器设置")
                        color: "#ffffff"
                        font.pixelSize: 24
                        font.weight: Font.DemiBold
                        Layout.fillWidth: true
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    height: 1
                    color: "#3e454a"
                }

                Text {
                    text: qsTr("图像辅助")
                    color: "#8f979d"
                    font.pixelSize: 14
                    font.weight: Font.DemiBold
                }

                RowLayout {
                    Layout.fillWidth: true

                    Text {
                        text: qsTr("伪色曝光")
                        color: "#f6f7f8"
                        font.pixelSize: 17
                        Layout.fillWidth: true
                    }

                    Switch {
                        checked: videoController.falseColorEnabled
                        palette.highlight: "#ffcf33"
                        onToggled: videoController.setFalseColorEnabled(checked)
                    }
                }

                Row {
                    Layout.alignment: Qt.AlignHCenter
                    spacing: 0

                    Repeater {
                        model: root.falseColorPalette

                        Rectangle {
                            width: 54
                            height: 18
                            color: modelData
                        }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true

                    Switch {
                        text: qsTr("斑马纹")
                        checked: videoController.zebraEnabled
                        palette.text: "#f6f7f8"
                        palette.highlight: "#ffcf33"
                        onToggled: videoController.setZebraEnabled(checked)
                    }

                    Slider {
                        id: zebraSlider
                        Layout.fillWidth: true
                        from: 0
                        to: 100
                        stepSize: 1
                        value: videoController.zebraLevel
                        enabled: videoController.zebraEnabled
                        onPressedChanged: {
                            if (!pressed)
                                videoController.setZebraLevel(Math.round(value))
                        }
                    }

                    Text {
                        text: Math.round(zebraSlider.value) + "%"
                        color: "#f6f7f8"
                        font.pixelSize: 14
                        Layout.preferredWidth: 44
                        horizontalAlignment: Text.AlignRight
                    }
                }

                RowLayout {
                    Layout.fillWidth: true

                    Switch {
                        text: qsTr("峰值")
                        checked: videoController.peakingEnabled
                        palette.text: "#f6f7f8"
                        palette.highlight: "#ff3440"
                        onToggled: videoController.setPeakingEnabled(checked)
                    }

                    Slider {
                        id: peakingSlider
                        Layout.fillWidth: true
                        from: 1
                        to: 100
                        stepSize: 1
                        value: videoController.peakingSensitivity
                        enabled: videoController.peakingEnabled
                        onPressedChanged: {
                            if (!pressed)
                                videoController.setPeakingSensitivity(Math.round(value))
                        }
                    }

                    Text {
                        text: Math.round(peakingSlider.value)
                        color: "#f6f7f8"
                        font.pixelSize: 14
                        Layout.preferredWidth: 44
                        horizontalAlignment: Text.AlignRight
                    }
                }

                Switch {
                    text: qsTr("亮度波形图")
                    checked: videoController.waveformEnabled
                    Layout.fillWidth: true
                    palette.text: "#f6f7f8"
                    palette.highlight: "#5ae884"
                    onToggled: videoController.setWaveformEnabled(checked)
                }

                RowLayout {
                    Layout.fillWidth: true

                    Text {
                        text: qsTr("中心放大")
                        color: "#f6f7f8"
                        font.pixelSize: 17
                        Layout.fillWidth: true
                    }

                    Row {
                        spacing: 1

                        Repeater {
                            model: [1, 2, 4]

                            Button {
                                id: zoomButton
                                width: 62
                                height: 40
                                text: modelData + "x"
                                onClicked: videoController.setZoomFactor(modelData)
                                background: Rectangle {
                                    color: videoController.zoomFactor === modelData
                                           ? "#3c788c" : "#272c30"
                                    border.width: 1
                                    border.color: "#4a5157"
                                }
                                contentItem: Text {
                                    text: zoomButton.text
                                    color: "#f6f7f8"
                                    font.pixelSize: 15
                                    font.weight: videoController.zoomFactor === modelData
                                                 ? Font.DemiBold : Font.Normal
                                    horizontalAlignment: Text.AlignHCenter
                                    verticalAlignment: Text.AlignVCenter
                                }
                            }
                        }
                    }
                }

                Switch {
                    text: qsTr("中心标记")
                    checked: root.centerMarkerEnabled
                    Layout.fillWidth: true
                    palette.text: "#f6f7f8"
                    palette.highlight: "#58d5ff"
                    onToggled: root.centerMarkerEnabled = checked
                }

                Switch {
                    text: qsTr("90% 安全框")
                    checked: root.safeAreaEnabled
                    Layout.fillWidth: true
                    palette.text: "#f6f7f8"
                    palette.highlight: "#58d5ff"
                    onToggled: root.safeAreaEnabled = checked
                }

                Switch {
                    text: qsTr("三分网格")
                    checked: root.gridEnabled
                    Layout.fillWidth: true
                    palette.text: "#f6f7f8"
                    palette.highlight: "#58d5ff"
                    onToggled: root.gridEnabled = checked
                }

                Switch {
                    text: qsTr("2.39:1 画幅线")
                    checked: root.frameGuideEnabled
                    Layout.fillWidth: true
                    palette.text: "#f6f7f8"
                    palette.highlight: "#58d5ff"
                    onToggled: root.frameGuideEnabled = checked
                }

                RowLayout {
                    Layout.fillWidth: true

                    Text {
                        text: qsTr("画面适配")
                        color: "#f6f7f8"
                        font.pixelSize: 17
                        Layout.fillWidth: true
                    }

                    Button {
                        width: 120
                        height: 44
                        text: videoController.fillModeText
                        onClicked: videoController.toggleFillMode()
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    height: 1
                    color: "#3e454a"
                }

                Text {
                    text: qsTr("输入状态")
                    color: "#8f979d"
                    font.pixelSize: 14
                    font.weight: Font.DemiBold
                }

                GridLayout {
                    Layout.fillWidth: true
                    columns: 2
                    columnSpacing: 24
                    rowSpacing: 14

                    Text { text: qsTr("分辨率"); color: "#9ca3a8"; font.pixelSize: 15 }
                    Text {
                        text: hdmiController.signalPresent
                              ? hdmiController.width + " x " + hdmiController.height : "--"
                        color: "#f6f7f8"
                        font.pixelSize: 15
                    }
                    Text { text: qsTr("帧率"); color: "#9ca3a8"; font.pixelSize: 15 }
                    Text {
                        text: hdmiController.signalPresent
                              ? hdmiController.frameRateText + "p" : "--"
                        color: "#f6f7f8"
                        font.pixelSize: 15
                    }
                    Text { text: qsTr("格式"); color: "#9ca3a8"; font.pixelSize: 15 }
                    Text { text: hdmiController.pixelFormat; color: "#f6f7f8"; font.pixelSize: 15 }
                    Text { text: qsTr("色彩"); color: "#9ca3a8"; font.pixelSize: 15 }
                    Text { text: hdmiController.colorimetry; color: "#f6f7f8"; font.pixelSize: 15 }
                }

                Item { Layout.fillHeight: true }

                Text {
                    text: qsTr("丢弃旧帧：") + videoController.droppedFrames
                    color: "#767e84"
                    font.pixelSize: 13
                }
            }
        }

        Rectangle {
            id: cameraPanel
            width: 570
            anchors.right: rightRail.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            color: "#ed111417"
            border.width: 1
            border.color: "#4a5157"
            visible: root.hudVisible && root.activePanel === "camera"
            z: 30

            ScrollView {
                id: cameraScroll
                anchors.fill: parent
                anchors.margins: 30
                clip: true

                ColumnLayout {
                    width: cameraScroll.availableWidth
                    spacing: 16

                    RowLayout {
                        Layout.fillWidth: true

                        Text {
                            text: qsTr("相机控制")
                            color: "#ffffff"
                            font.pixelSize: 24
                            font.weight: Font.DemiBold
                            Layout.fillWidth: true
                        }

                        Button {
                            id: closeCameraButton
                            width: 46
                            height: 46
                            text: "×"
                            onClicked: root.closePanels()
                            background: Rectangle { color: "transparent" }
                            contentItem: Text {
                                text: closeCameraButton.text
                                color: "#f6f7f8"
                                font.pixelSize: 34
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        height: 1
                        color: "#3e454a"
                    }

                    RowLayout {
                        Layout.fillWidth: true

                        ColumnLayout {
                            spacing: 4
                            Layout.fillWidth: true

                            Text {
                                text: cameraController.connectionText
                                color: cameraController.connected ? "#5de176" : "#e1a143"
                                font.pixelSize: 17
                                font.weight: Font.DemiBold
                            }

                            Text {
                                text: cameraController.connected
                                      ? cameraController.modelName + "  "
                                        + cameraController.firmwareVersion : qsTr("等待相机")
                                color: "#9ca3a8"
                                font.pixelSize: 14
                            }
                        }

                        Button {
                            text: qsTr("重连")
                            visible: !cameraController.connected
                            onClicked: cameraController.reconnect()
                        }
                    }

                    GridLayout {
                        Layout.fillWidth: true
                        columns: 2
                        columnSpacing: 18
                        rowSpacing: 13
                        visible: cameraController.connected

                        Text {
                            text: qsTr("ISO")
                            color: "#aeb5ba"
                            visible: cameraController.isoOptions.length > 0
                        }
                        ComboBox {
                            Layout.fillWidth: true
                            model: cameraController.isoOptions
                            textRole: "label"
                            currentIndex: root.optionIndex(model, cameraController.isoValue)
                            visible: cameraController.isoOptions.length > 0
                            palette.button: "#272c30"
                            palette.buttonText: "#f6f7f8"
                            palette.text: "#f6f7f8"
                            palette.highlight: "#3c788c"
                            onActivated: cameraController.setIso(model[index].value)
                        }

                        Text {
                            text: qsTr("快门")
                            color: "#aeb5ba"
                            visible: cameraController.shutterOptions.length > 0
                        }
                        ComboBox {
                            Layout.fillWidth: true
                            model: cameraController.shutterOptions
                            textRole: "label"
                            currentIndex: root.optionIndex(model, cameraController.shutterValue)
                            visible: cameraController.shutterOptions.length > 0
                            palette.button: "#272c30"
                            palette.buttonText: "#f6f7f8"
                            palette.text: "#f6f7f8"
                            palette.highlight: "#3c788c"
                            onActivated: cameraController.setShutterSpeed(model[index].value)
                        }

                        Text {
                            text: qsTr("光圈")
                            color: "#aeb5ba"
                            visible: cameraController.apertureOptions.length > 0
                        }
                        ComboBox {
                            Layout.fillWidth: true
                            model: cameraController.apertureOptions
                            textRole: "label"
                            currentIndex: root.optionIndex(model, cameraController.apertureValue)
                            visible: cameraController.apertureOptions.length > 0
                            palette.button: "#272c30"
                            palette.buttonText: "#f6f7f8"
                            palette.text: "#f6f7f8"
                            palette.highlight: "#3c788c"
                            onActivated: cameraController.setAperture(model[index].value)
                        }

                        Text {
                            text: qsTr("白平衡")
                            color: "#aeb5ba"
                            visible: cameraController.whiteBalanceOptions.length > 0
                        }
                        ComboBox {
                            Layout.fillWidth: true
                            model: cameraController.whiteBalanceOptions
                            textRole: "label"
                            currentIndex: root.optionIndex(model, cameraController.whiteBalanceValue)
                            visible: cameraController.whiteBalanceOptions.length > 0
                            palette.button: "#272c30"
                            palette.buttonText: "#f6f7f8"
                            palette.text: "#f6f7f8"
                            palette.highlight: "#3c788c"
                            onActivated: cameraController.setWhiteBalance(model[index].value)
                        }

                        Text {
                            text: qsTr("对焦模式")
                            color: "#aeb5ba"
                            visible: cameraController.focusModeOptions.length > 0
                        }
                        ComboBox {
                            Layout.fillWidth: true
                            model: cameraController.focusModeOptions
                            textRole: "label"
                            currentIndex: root.optionIndex(model, cameraController.focusModeValue)
                            visible: cameraController.focusModeOptions.length > 0
                            palette.button: "#272c30"
                            palette.buttonText: "#f6f7f8"
                            palette.text: "#f6f7f8"
                            palette.highlight: "#3c788c"
                            onActivated: cameraController.setFocusMode(model[index].value)
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        visible: cameraController.canAutoFocus
                                 || cameraController.canTouchFocus

                        Button {
                            text: qsTr("按住自动对焦")
                            Layout.fillWidth: true
                            visible: cameraController.canAutoFocus
                            onPressed: cameraController.setAutoFocus(true)
                            onReleased: cameraController.setAutoFocus(false)
                            onCanceled: cameraController.setAutoFocus(false)
                        }

                        Button {
                            text: qsTr("触控对焦")
                            Layout.fillWidth: true
                            visible: cameraController.canTouchFocus
                            onClicked: {
                                root.closePanels()
                                root.touchFocusMode = true
                                root.hudVisible = false
                                hudTimer.stop()
                            }
                        }
                    }

                    Text {
                        Layout.fillWidth: true
                        text: cameraController.lastError
                        color: "#e1a143"
                        font.pixelSize: 14
                        wrapMode: Text.Wrap
                        visible: text.length > 0
                    }
                }
            }
        }
    }
}
