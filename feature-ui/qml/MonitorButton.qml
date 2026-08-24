import QtQuick 2.15
import QtQuick.Controls 2.15

Button {
    id: control

    property url iconSource
    property bool active: false
    property color accentColor: "#58d5ff"
    property string toolTipText: text

    width: 84
    height: 76
    padding: 0

    background: Rectangle {
        radius: 2
        color: !control.enabled ? "#241f2225"
                                : control.down ? "#cc40464b"
                                : control.active ? "#d93a4147"
                                                 : "transparent"

        Rectangle {
            width: 28
            height: 3
            radius: 1
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            color: control.accentColor
            visible: control.active && control.enabled
        }
    }

    contentItem: Item {
        Image {
            id: icon
            width: 25
            height: 25
            anchors.top: parent.top
            anchors.topMargin: 11
            anchors.horizontalCenter: parent.horizontalCenter
            source: control.iconSource
            sourceSize: Qt.size(25, 25)
            fillMode: Image.PreserveAspectFit
            opacity: control.enabled ? 1 : 0.38
        }

        Text {
            width: parent.width - 8
            height: 22
            anchors.top: icon.bottom
            anchors.topMargin: 5
            anchors.horizontalCenter: parent.horizontalCenter
            text: control.text
            color: control.enabled ? "#f6f7f8" : "#73787d"
            font.pixelSize: 13
            font.weight: control.active ? Font.DemiBold : Font.Normal
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }
    }
}
