import QtQuick
import QtQuick.Controls
import LayoutDesigner 1.0

// Layout Designer canvas: corridors (blue), rooms (green) and vents (orange) on a top-down grid.
Rectangle {
    id: root
    color: "#101428"

    property var m: layoutModel
    readonly property real sf: ctx ? ctx.scaleFactor : 1.0

    LayoutCanvas {
        id: canvas
        anchors.fill: parent
        model: root.m
        focus: true
    }

    Rectangle {
        id: strip
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.margins: 10
        width: column.width + 8
        height: column.height + 8
        radius: 5
        color: "#e6101428"
        border.color: "#346077"
        border.width: 1

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
        }

        Column {
            id: column
            anchors.centerIn: parent
            spacing: 2

            Repeater {
                model: [
                    { t: 0, name: "Select", key: "1", icon: "restrict_select_off",
                      tip: "Select segments and rooms" },
                    { t: 1, name: "Corridor", key: "2", icon: "ipo_linear",
                      tip: "Draw a corridor" },
                    { t: 2, name: "Room", key: "3", icon: "mesh_plane",
                      tip: "Draw a room" },
                    { t: 3, name: "Vent", key: "4", icon: "force_wind",
                      tip: "Draw a vent duct" },
                    { t: 4, name: "Seal", key: "5", icon: "locked",
                      tip: "Seal a grid cell with walls" },
                    { t: 5, name: "Erase", key: "6", icon: "cursor_eraser",
                      tip: "Erase segments, rooms and seals" }
                ]
                delegate: Rectangle {
                    id: btn
                    required property var modelData
                    readonly property bool active: root.m && root.m.tool === modelData.t
                    width: 66
                    height: 54
                    radius: 4
                    color: active ? "#4866C5" : (mouse.containsMouse ? "#232A4E" : "transparent")
                    border.color: active || mouse.containsMouse ? "#346077" : "transparent"
                    border.width: 1

                    Image {
                        id: icon
                        anchors.horizontalCenter: parent.horizontalCenter
                        anchors.top: parent.top
                        anchors.topMargin: 5
                        width: 26
                        height: 26
                        sourceSize: Qt.size(52, 52)
                        source: "qrc:/icons_blender/" + btn.modelData.icon + ".svg"
                        smooth: true
                    }
                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        anchors.top: icon.bottom
                        anchors.topMargin: 3
                        text: btn.modelData.name
                        color: btn.active ? "#ffffff" : "#dfe4f5"
                        font.pixelSize: 12
                        font.bold: true
                    }
                    MouseArea {
                        id: mouse
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: {
                            root.m.tool = btn.modelData.t
                            canvas.forceActiveFocus()
                        }
                    }
                    ToolTip.visible: mouse.containsMouse
                    ToolTip.delay: 500
                    ToolTip.text: "<b>" + modelData.name + "</b>  <span style='color:#8a9ab8'>" + modelData.key + "</span><br>" + modelData.tip
                }
            }
        }
    }
}
