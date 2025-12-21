import QtQuick

// Hand-drawn checkbox sized off ctx.scaleFactor: the styled Controls one keeps its own metrics.
Rectangle {
    id: control

    property string text: ""
    property bool checked: false
    property color textColor: "#c7cce7"
    property color labelColorChecked: "#ffffff"

    readonly property real sf: ctx ? ctx.scaleFactor : 1.0
    // The label follows UI × font scale.
    readonly property real tf: ctx && ctx.textScaleFactor ? ctx.textScaleFactor : sf
    property int fontPx: 14                 // at UI and font scale 1
    readonly property real boxSize: Math.round(16 * tf)

    signal toggled()

    implicitWidth: boxSize + 8 * sf + label.implicitWidth
    implicitHeight: Math.max(boxSize, label.implicitHeight)
    color: "transparent"

    function toggle() {
        control.checked = !control.checked
        control.toggled()
    }

    Rectangle {
        id: box
        width: control.boxSize
        height: control.boxSize
        y: (control.height - height) / 2
        radius: Math.max(2, 3 * control.sf)
        border.width: Math.max(1, control.sf)
        border.color: control.checked ? "#d98a95"
                                    : (mouse.containsMouse ? "#8a9ac0" : "#4a5a7a")
        color: control.checked ? "#8a3b46"
                              : (mouse.containsMouse ? "#2e3850" : "#20283a")

        Text {
            anchors.centerIn: parent
            visible: control.checked
            text: "\u2713"
            color: "#ffe9ec"
            font.pixelSize: Math.round(control.boxSize * 0.8)
            font.bold: true
        }
    }

    Text {
        id: label
        anchors.left: box.right
        anchors.leftMargin: 8 * control.sf
        anchors.verticalCenter: parent.verticalCenter
        text: control.text
        color: control.checked ? control.labelColorChecked : control.textColor
        font.pixelSize: Math.round(control.fontPx * control.tf)
    }

    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton
        cursorShape: Qt.PointingHandCursor
        onClicked: control.toggle()
    }
}
