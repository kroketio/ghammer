import QtQuick
import RhiView 1.0 as RV

// The Texture Browser's 3D preview (ui::MaterialPreview3D fills the scene and follows the view properties).
Rectangle {
    id: root

    property int textPx: 11            // caption size, from ui::scale
    property int pending: 0            // maps still downloading

    property real yaw: -30
    property real pitch: 22
    property real zoom: 1.0
    property real lightYaw: -40
    property real lightPitch: -45

    signal viewChanged()
    onYawChanged: viewChanged()
    onPitchChanged: viewChanged()
    onZoomChanged: viewChanged()
    onLightYawChanged: viewChanged()
    onLightPitchChanged: viewChanged()

    function resetView() {
        yaw = -30; pitch = 22; zoom = 1.0
        lightYaw = -40; lightPitch = -45
    }
    function clamp(v, lo, hi) { return Math.max(lo, Math.min(hi, v)) }

    color: "#0f1326"
    border.width: 1
    border.color: "#232A4E"

    RV.Viewport3D {
        objectName: "view"
        anchors.fill: parent
        anchors.margins: 1
    }

    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        cursorShape: pressed ? Qt.ClosedHandCursor : Qt.OpenHandCursor
        property point last

        onPressed: (m) => last = Qt.point(m.x, m.y)
        onPositionChanged: (m) => {
            const dx = m.x - last.x
            const dy = m.y - last.y
            last = Qt.point(m.x, m.y)
            if ((m.buttons & Qt.RightButton) || (m.modifiers & Qt.ShiftModifier)) {
                root.lightYaw += dx * 0.5
                root.lightPitch = root.clamp(root.lightPitch + dy * 0.5, -89, 60)
            } else {
                root.yaw -= dx * 0.5
                root.pitch = root.clamp(root.pitch + dy * 0.5, -89, 89)
            }
        }
        onWheel: (w) => root.zoom = root.clamp(root.zoom * Math.pow(0.999, w.angleDelta.y), 0.55, 2.5)
        onDoubleClicked: root.resetView()
    }

    Text {
        visible: root.pending > 0
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        anchors.margins: root.textPx * 0.8
        text: qsTr("loading maps…")
        color: "#cfd8ee"
        font.pixelSize: root.textPx
        style: Text.Outline
        styleColor: "#80000000"
    }
}
