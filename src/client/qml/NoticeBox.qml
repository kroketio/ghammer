import QtQuick

// One notice pill of the 3D view's message stack (ui::ViewportNotices). Never takes the mouse.
Rectangle {
    id: box

    property alias text: label.text
    property int fontPx: 15                 // at UI and font scale 1
    readonly property real sf: ctx ? ctx.scaleFactor : 1.0
    // Text follows the font scale too, like widget text.
    readonly property real tf: ctx && ctx.textScaleFactor ? ctx.textScaleFactor : sf

    width: label.implicitWidth + 36 * sf
    height: label.implicitHeight + 16 * sf
    radius: height / 2
    color: "#e01a2140"
    border.color: "#22a0ff"
    border.width: 1

    Text {
        id: label
        anchors.centerIn: parent
        color: "#e8eefc"
        font.pixelSize: Math.round(box.fontPx * box.tf)
    }
}
