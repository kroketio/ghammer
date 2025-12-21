import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    property alias textItem: textItem
    property alias text: textItem.text
    property var textColor: "#c7cce7"
    property var textActive: "#fdfdfd"
    property bool active: false
    property bool checked: false
    property real tf: 1.0 // text scale (ctx.textScaleFactor)

    signal clicked()

    radius: active || checked ? 2 : 0
    color: checked ? "#2c4a6b" : (active ? "#393e5e" : "transparent")
    border.color: checked ? "#5f8fc0" : "transparent"
    border.width: checked ? 1 : 0

    Text {
        id: textItem
        color: (parent.checked || parent.active) ? parent.textActive : parent.textColor
        font.pointSize: 15 * parent.tf
        anchors.centerIn: parent
    }

    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.AllButtons
        cursorShape: Qt.PointingHandCursor
        onEntered: parent.active = true
        onExited: parent.active = false
        onClicked: parent.clicked()
    }
}