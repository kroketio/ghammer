import QtQuick
import RhiView 1.0 as RV

// Right half of the project dialog: a turning brush level (built by ui::ProjectDialog) under the credits.
Rectangle {
    id: root

    property int textPx: 13            // body text size, from ui::scale
    property real angle: 0
    signal viewChanged()
    onAngleChanged: viewChanged()

    readonly property var credits: [
        ["Logo", "u/toadile on Reddit"],
        ["Qt 6", "LGPL-3.0"],
        ["Assimp", "BSD-3-Clause"],
        ["Box3D", "MIT"],
        ["QCoro", "MIT"],
        ["GLM", "MIT"],
        ["tinyexr", "BSD-3-Clause"],
        ["stb", "MIT / Public Domain"],
        ["libpng", "libpng"],
        ["zlib", "zlib"],
        ["SQLite", "Public Domain"],
        ["RapidJSON", "MIT"],
        ["sourcepp", "MIT"],
        ["nameof", "MIT"],
        ["HammerPro", "MIT"],
        ["Godot editor icons", "MIT"],
        ["Blender icons", "CC BY-SA 4.0"]
    ]

    color: "#0c1022"

    NumberAnimation on angle {
        from: 0; to: 360
        duration: 120000
        loops: Animation.Infinite
        running: root.visible
    }

    RV.Viewport3D {
        objectName: "view"
        anchors.fill: parent
    }

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: content.height + root.textPx * 7
        gradient: Gradient {
            GradientStop { position: 0.0; color: "#000c1022" }
            GradientStop { position: 0.45; color: "#b30c1022" }
            GradientStop { position: 1.0; color: "#f20c1022" }
        }
    }

    Column {
        id: content
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: root.textPx * 2
        spacing: root.textPx * 0.25

        Text {
            text: qsTr("MADE WITH THE HELP OF")
            color: "#ffffff"
            font.pixelSize: root.textPx * 0.75
            font.weight: Font.DemiBold
            font.letterSpacing: root.textPx * 0.12
        }
        Rectangle {
            width: parent.width
            height: 1
            color: "#ffffff"
        }
        Grid {
            id: grid
            readonly property real cellWidth: root.textPx * 14
            width: parent.width
            columns: Math.max(1, Math.floor(width / cellWidth))
            rowSpacing: root.textPx * 0.3
            topPadding: root.textPx * 0.3
            Repeater {
                model: root.credits
                Row {
                    width: grid.cellWidth
                    spacing: root.textPx * 0.45
                    Text {
                        text: modelData[0]
                        color: "#ffffff"
                        font.pixelSize: root.textPx * 0.85
                        font.weight: Font.Bold
                    }
                    Text {
                        text: modelData[1]
                        color: "#ffffff"
                        font.pixelSize: root.textPx * 0.85
                        anchors.baseline: parent.children[0].baseline
                    }
                }
            }
        }
    }
}
