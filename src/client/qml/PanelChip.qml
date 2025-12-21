import QtQuick
import QtQuick.Controls

// One button of a segmented row in a viewport panel; `active` paints it in the accent colour.
Rectangle {
    id: chip

    property string text: ""
    property bool active: false
    property color accent: "#3b7a5c"
    property color accentBorder: "#7fcf9f"
    property real fontScale: 1.0
    property string tip: ""
    readonly property real sf: ctx ? ctx.scaleFactor : 1.0

    signal clicked()

    implicitHeight: 24 * sf
    implicitWidth: label.implicitWidth + 14 * sf
    radius: 4
    color: active ? accent : (mouse.containsMouse ? "#33405c" : "#232c3d")
    border.color: active ? accentBorder : "#3f4c68"
    border.width: 1
    opacity: enabled ? 1.0 : 0.45

    Text {
        id: label
        anchors.centerIn: parent
        color: chip.active ? "#ffffff" : "#b9c4d8"
        font.pointSize: 9 * chip.sf * chip.fontScale
        font.bold: chip.active
        text: chip.text
    }

    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: chip.clicked()
    }

    ToolTip.visible: mouse.containsMouse && chip.tip !== ""
    ToolTip.delay: 500
    ToolTip.text: chip.tip
}
