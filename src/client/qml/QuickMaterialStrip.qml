import QtQuick

// Quick Material strip (ui::QuickMaterialModel, context property quickMaterials): nearby materials, a divider,
// then recent ones. While open it has the view's mouse and keyboard, like the pie.
Item {
    id: strip

    anchors.fill: parent
    visible: false

    readonly property real sf: ctx ? ctx.scaleFactor : 1.0
    readonly property real tf: ctx && ctx.textScaleFactor ? ctx.textScaleFactor : sf
    readonly property real tile: 64 * sf
    readonly property real margin: 8 * sf

    property real cx: 0
    property real cy: 0
    property int hot: -1
    property string hotName: ""
    property real progress: 0

    function open(x, y) {
        cx = x
        cy = y
        hot = -1
        hotName = ""
        visible = true
        pieMenu.stripOpen = true
        if (typeof widget !== "undefined" && widget)
            widget.setUiOverlay(strip, true)
        progress = 0
        fadeIn.restart()
    }

    function close() {
        if (!visible)
            return
        fadeIn.stop()
        visible = false
        hot = -1
        pieMenu.stripOpen = false
        if (typeof widget !== "undefined" && widget)
            widget.setUiOverlay(strip, false)
    }

    function tileAt(mx, my) {
        for (let i = 0; i < tiles.count; ++i) {
            const box = tiles.itemAt(i).box
            const p = box.mapFromItem(strip, mx, my)
            if (box.contains(p))
                return i
        }
        return -1
    }

    NumberAnimation {
        id: fadeIn
        target: strip
        property: "progress"
        from: 0
        to: 1
        duration: 220
        easing.type: Easing.OutQuart
    }

    Connections {
        target: pieMenu
        function onQuickMaterialRequested(x, y) { strip.open(x, y) }
        function onCloseRequested() { strip.close() }
    }
    Connections {
        target: ctx
        function onEditorModeChanged() { strip.close() }
    }

    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton | Qt.RightButton | Qt.MiddleButton
        cursorShape: strip.hot >= 0 ? Qt.PointingHandCursor : Qt.ArrowCursor
        onPositionChanged: (mouse) => {
            strip.hot = strip.tileAt(mouse.x, mouse.y)
            strip.hotName = strip.hot >= 0 ? tiles.itemAt(strip.hot).name : ""
        }
        onPressed: (mouse) => {
            if (mouse.button !== Qt.LeftButton)
                strip.close()
        }
        onClicked: (mouse) => {
            if (mouse.button !== Qt.LeftButton)
                return
            const row = strip.tileAt(mouse.x, mouse.y)
            strip.close()
            if (row >= 0)
                quickMaterials.apply(row)
        }
        onWheel: (wheel) => { wheel.accepted = true }
    }

    Rectangle {
        id: panel
        width: row.implicitWidth + 16 * strip.sf
        height: row.implicitHeight + nameLabel.height + 20 * strip.sf
        x: Math.max(strip.margin, Math.min(strip.width - strip.margin - width, strip.cx - width / 2))
        y: Math.max(strip.margin, Math.min(strip.height - strip.margin - height, strip.cy - height / 2))
        opacity: strip.progress
        scale: 0.85 + 0.15 * strip.progress
        radius: 6 * strip.sf
        color: "#ee141927"
        border.color: "#2a3350"
        border.width: 1

        Row {
            id: row
            x: 8 * strip.sf
            y: 8 * strip.sf
            spacing: 6 * strip.sf
            Repeater {
                id: tiles
                model: quickMaterials
                delegate: Row {
                    id: entry
                    required property int index
                    required property string name
                    required property string thumb
                    required property bool firstRecent
                    property alias box: box
                    spacing: 6 * strip.sf

                    // Between the nearby materials and the recent ones.
                    Rectangle {
                        visible: entry.firstRecent && entry.index > 0
                        width: 1
                        height: strip.tile
                        color: "#3a4560"
                    }
                    Rectangle {
                        id: box
                        width: strip.tile
                        height: strip.tile
                        radius: 3 * strip.sf
                        color: "#0f1320"
                        border.color: strip.hot === entry.index ? "#4866C5" : "#2a3350"
                        border.width: strip.hot === entry.index ? 2 : 1
                        clip: true

                        Image {
                            id: image
                            anchors.fill: parent
                            anchors.margins: 2
                            source: entry.thumb
                            cache: false
                            asynchronous: false
                            fillMode: Image.PreserveAspectCrop
                            sourceSize: Qt.size(128, 128)
                        }
                        // While the thumbnail loads (or for built-in ones).
                        Text {
                            anchors.centerIn: parent
                            visible: image.status !== Image.Ready || image.implicitWidth <= 1
                            text: entry.name.length > 0 ? entry.name.charAt(0).toUpperCase() : "?"
                            color: "#5a6788"
                            font.pixelSize: Math.round(22 * strip.tf)
                        }
                    }
                }
            }
        }
        Text {
            id: nameLabel
            anchors.horizontalCenter: parent.horizontalCenter
            y: row.y + row.implicitHeight + 6 * strip.sf
            width: Math.min(implicitWidth, panel.width - 16 * strip.sf)
            elide: Text.ElideRight
            text: strip.hotName.length > 0 ? strip.hotName : " "
            color: "#c8d0e4"
            font.pixelSize: Math.round(12 * strip.tf)
        }
    }
}
