import QtQuick
import QtQuick.Controls

// Square toggle for the viewport menu bar; active = the material panel's light blue border.
Rectangle {
    id: button

    property alias text: label.text
    property string tip: ""
    property bool checked: false
    property color accentColor: "#22a0ff"
    property real tf: 1.0 // text scale (ctx.textScaleFactor)

    readonly property bool highlighted: mouse.containsMouse

    signal clicked()

    radius: 3
    antialiasing: true
    color: checked ? (highlighted ? "#33567c" : "#2c4a6b")
                   : (highlighted ? "#393e5e" : "transparent")
    border.width: checked ? 2 : 1
    border.color: checked ? accentColor : "#4a5a7a"

    Text {
        id: label
        anchors.centerIn: parent
        font.pointSize: 12 * button.tf
        font.bold: true
        color: button.checked ? "#fdfdfd" : (button.highlighted ? "#e6e9f5" : "#c7cce7")
    }

    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: button.clicked()
        ToolTip.visible: button.tip !== "" && containsMouse
        ToolTip.delay: 600
        ToolTip.text: button.tip
    }
}
