import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Vertex scale panel (vertex tool): the selected corners scale about the magenta pivot.
Rectangle {
    id: panel

    property var engine: engine3D
    readonly property real sf: ctx ? ctx.scaleFactor : 1.0
    readonly property color accent: "#6a2f6a"
    readonly property color accentBorder: "#e07fe0"

    width: 290 * sf
    height: col.implicitHeight + 18 * sf
    radius: 6
    color: "#e6221628"
    border.color: "#8a5a8a"
    border.width: 1

    PanelInputBlocker {
        name: "vertex scale panel"
    }

    ColumnLayout {
        id: col
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 9 * panel.sf
        spacing: 7 * panel.sf

        RowLayout {
            Layout.fillWidth: true
            Text {
                text: "SCALE VERTICES"
                color: "#f5dff5"
                font.pointSize: 10 * panel.sf
                font.bold: true
            }
            Text {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignRight
                color: "#c08fc0"
                font.pointSize: 9 * panel.sf
                text: panel.engine ? engine.selectedVertexCount + " corners" : ""
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 6 * panel.sf
            Text {
                Layout.preferredWidth: 50 * panel.sf
                color: "#c7cce7"
                font.pointSize: 9 * panel.sf
                text: "scale"
            }
            PanelChip {
                text: "−0.1"
                accent: panel.accent
                accentBorder: panel.accentBorder
                onClicked: if (panel.engine) engine.vertexScale = Math.max(0.01, engine.vertexScale - 0.1)
            }
            TextField {
                id: field
                Layout.fillWidth: true
                Layout.preferredHeight: 24 * panel.sf
                font.pointSize: 10 * panel.sf
                horizontalAlignment: Text.AlignHCenter
                color: "#ffffff"
                background: Rectangle { color: "#1b2233"; border.color: "#4a5a7a"; radius: 3 }
                text: panel.engine ? engine.vertexScale.toFixed(2) : "1.00"
                validator: DoubleValidator { bottom: 0.01; top: 100.0 }
                onEditingFinished: if (panel.engine) engine.vertexScale = parseFloat(text)
            }
            PanelChip {
                text: "+0.1"
                accent: panel.accent
                accentBorder: panel.accentBorder
                onClicked: if (panel.engine) engine.vertexScale = engine.vertexScale + 0.1
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 4 * panel.sf
            Text {
                Layout.preferredWidth: 50 * panel.sf
                color: "#c7cce7"
                font.pointSize: 9 * panel.sf
                text: "axes"
            }
            Repeater {
                model: [ { bit: 1, name: "X" }, { bit: 2, name: "Y" }, { bit: 4, name: "Z" } ]
                delegate: PanelChip {
                    required property var modelData
                    Layout.fillWidth: true
                    text: modelData.name
                    active: panel.engine && (panel.engine.vertexScaleAxes & modelData.bit) !== 0
                    accent: panel.accent
                    accentBorder: panel.accentBorder
                    onClicked: if (panel.engine) engine.vertexScaleAxes = engine.vertexScaleAxes ^ modelData.bit
                }
            }
            PanelChip {
                text: "centre pivot"
                tip: "Centre the pivot"
                onClicked: if (panel.engine) engine.resetVertexPivot()
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 6 * panel.sf
            PanelChip {
                Layout.fillWidth: true
                text: "Apply  ⏎"
                active: true
                accent: panel.accent
                accentBorder: panel.accentBorder
                onClicked: if (panel.engine) engine.commitVertexScale()
            }
            PanelChip {
                Layout.fillWidth: true
                text: "Cancel  Esc"
                onClicked: if (panel.engine) engine.cancelVertexScale()
            }
        }
    }
}
