import QtQuick
import QtQuick.Controls
import RhiView 1.0 as RV

Rectangle {
    id: root

    property alias perspective: gridView.perspective
    property alias showGrid: gridView.showGrid
    property string title: "Top"
    // UI scale (sizes) and text scale (UI × text size), as the 3D view's overlays.
    readonly property real sf: typeof ctx !== "undefined" && ctx ? ctx.scaleFactor : 1.0
    readonly property real tf: typeof ctx !== "undefined" && ctx && ctx.textScaleFactor ? ctx.textScaleFactor : sf

    color: "#151922"

    RV.GridView {
        id: gridView
        anchors.fill: parent
        cursorTarget: mouseArea
    }

    MouseArea {
        id: mouseArea
        anchors.fill: parent
        acceptedButtons: Qt.MiddleButton | Qt.LeftButton | Qt.RightButton
        hoverEnabled: true
        cursorShape: gridView.hoverCursor
        property real lastX: 0.0
        property real lastY: 0.0
        // Shift = add to the selection; latched on press so letting go of shift first still finishes the click.
        property bool addSelection: false

        // Hover is consumed here so it never reaches the 3D view's gizmo hover.
        onExited: gridView.endHover()

        onWheel: function (wheel) {
            var delta = wheel.angleDelta.y
            if (delta === 0)
                delta = wheel.pixelDelta.y * 30
            if (delta === 0) {
                wheel.accepted = false
                return
            }
            // Ctrl+wheel steps the shared grid level (up finer), same ladder as [ and ].
            if (wheel.modifiers & Qt.ControlModifier) {
                gridView.stepGridLevel(delta > 0 ? -1 : 1)
                wheel.accepted = true
                return
            }
            gridView.zoomAt(wheel.x, wheel.y, Math.pow(1.3, delta / 120))
            wheel.accepted = true
        }

        onPressed: function (event) {
            lastX = event.x
            lastY = event.y
            addSelection = (event.modifiers & Qt.ShiftModifier) !== 0
            if (event.button === Qt.LeftButton)
                gridView.beginLeftDrag(event.x, event.y, addSelection)
        }

        // Right-click menu, opened on release so the press never starts anything else.
        onClicked: function (event) {
            if (event.button === Qt.RightButton && typeof window !== "undefined" && window) {
                const at = mapToItem(null, event.x, event.y)
                window.showGridContextMenu(at.x, at.y)
            }
        }

        onPositionChanged: function (event) {
            if (event.buttons & Qt.MiddleButton) {
                gridView.panBy(event.x - lastX, event.y - lastY)
                lastX = event.x
                lastY = event.y
            } else if (event.buttons & Qt.LeftButton) {
                // Shift read live: pressed mid-drag it turns the move into a duplicate.
                gridView.updateLeftDrag(event.x, event.y,
                                      (event.modifiers & Qt.ShiftModifier) !== 0)
            } else {
                gridView.hoverAt(event.x, event.y)
            }
        }

        onReleased: function (event) {
            if (event.button === Qt.LeftButton)
                gridView.endLeftDrag(event.x, event.y, addSelection)
        }

        // Finishes a path drawn with the pen.
        onDoubleClicked: function (event) {
            if (event.button === Qt.LeftButton)
                gridView.doubleClickAt(event.x, event.y)
        }
    }

    // The arch panel rides the 2D view the marquee was drawn in.
    VertexScaleControls {
        z: 10
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        anchors.leftMargin: 12
        anchors.bottomMargin: 12
        visible: engine3D && engine3D.vertexScaleActive
                 && engine3D.vertexEditPlane === Number(gridView.perspective)
    }

    Rectangle {
        id: hud
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.margins: 8 * root.sf
        radius: 4 * root.sf
        color: "#66000000"
        width: hudRow.implicitWidth + 16 * root.sf
        height: hudRow.implicitHeight + 8 * root.sf

        Row {
            id: hudRow
            anchors.centerIn: parent
            spacing: 12 * root.sf

            Text {
                anchors.verticalCenter: parent.verticalCenter
                color: "#d3dbe8"
                font.pixelSize: Math.round(13 * root.tf)
                font.bold: true
                text: root.title
            }

            Text {
                anchors.verticalCenter: parent.verticalCenter
                color: "#8b97ab"
                font.pixelSize: Math.round(12 * root.tf)
                text: "grid L" + gridView.gridLevel + " - " + (gridView.gridStepSource >= 1
                        ? Math.round(gridView.gridStepSource)
                        : gridView.gridStepSource.toFixed(3).replace(/0+$/, "").replace(/\.$/, "")) + " units"
            }

            Text {
                anchors.verticalCenter: parent.verticalCenter
                visible: text.length > 0
                color: "#ff7a6a"
                font.pixelSize: Math.round(12 * root.tf)
                font.bold: true
                text: typeof engine3D !== "undefined" && engine3D ? engine3D.cutModeLabel : ""
            }

            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                radius: 3 * root.sf
                color: fitMouse.containsMouse ? "#2f3b52" : "#232c3d"
                width: fitText.implicitWidth + 14 * root.sf
                height: fitText.implicitHeight + 6 * root.sf

                Text {
                    id: fitText
                    anchors.centerIn: parent
                    color: "#c3cddd"
                    font.pixelSize: Math.round(12 * root.tf)
                    text: "fit"
                }

                MouseArea {
                    id: fitMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: gridView.fitToScene()
                }
            }
        }
    }
}
