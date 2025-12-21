import QtQuick
import QtQuick.Shapes

// Pie menu (ui::PieMenu, context property pieMenu): the mode's one-step commands on a ring, Blender style.
// While open it has the view's mouse (widget.setUiOverlay) and, through PieMenu::handleKey, the keyboard.
Item {
    id: pie

    anchors.fill: parent
    visible: false

    readonly property real sf: ctx ? ctx.scaleFactor : 1.0
    readonly property real tf: ctx && ctx.textScaleFactor ? ctx.textScaleFactor : sf
    readonly property real buttonHeight: 32 * sf
    readonly property real reach: 125 * sf      // centre to each button's inner edge
    readonly property real ringOuter: 22 * sf   // also the dead zone
    readonly property real ringWidth: 7 * sf
    readonly property real margin: 8 * sf

    property real cx: 0
    property real cy: 0
    property string title: ""
    property var entries: []
    property int hot: -1           // entries index under the cursor's direction
    property bool aimed: false     // the cursor is outside the dead zone
    property real aimAngle: 0      // degrees, clockwise from east (y down)
    property real progress: 0

    // Numpad slot -> unit direction, y down: 7 8 9 / 4 6 / 1 2 3.
    function slotDir(n) {
        const dx = [0, -1, 0, 1, -1, 0, 1, -1, 0, 1][n]
        const dy = [0, 1, 1, 1, 0, 0, 0, -1, -1, -1][n]
        const l = Math.sqrt(dx * dx + dy * dy) || 1
        return Qt.point(dx / l, dy / l)
    }
    // A button's top-left relative to the centre: the edge facing the centre
    // sits on the circle (sides: inner edge, top / bottom: centred).
    function relPos(d, w, h) {
        const ax = d.x * reach, ay = d.y * reach
        const x = d.x < -0.01 ? ax - w : (d.x > 0.01 ? ax : ax - w / 2)
        const y = Math.abs(d.x) > 0.01 ? ay - h / 2 : (d.y < 0 ? ay - h : ay)
        return Qt.point(x, y)
    }

    function open(x, y, t) {
        const list = pieMenu.items()
        if (list.length === 0)
            return
        entries = list
        title = t
        hot = -1
        aimed = false
        // Keep every button inside the view.
        let l = -ringOuter, r = ringOuter, tp = -ringOuter - titleLabel.height - 6 * sf, b = ringOuter
        for (let i = 0; i < buttons.count; ++i) {
            const it = buttons.itemAt(i)
            const p = relPos(slotDir(entries[i].number), it.width, it.height)
            l = Math.min(l, p.x); r = Math.max(r, p.x + it.width)
            tp = Math.min(tp, p.y); b = Math.max(b, p.y + it.height)
        }
        cx = Math.max(margin - l, Math.min(width - margin - r, x))
        cy = Math.max(margin - tp, Math.min(height - margin - b, y))
        visible = true
        pieMenu.pieOpen = true
        if (typeof widget !== "undefined" && widget)
            widget.setUiOverlay(pie, true)
        progress = 0
        flyOut.restart()
    }

    function close() {
        if (!visible)
            return
        flyOut.stop()
        visible = false
        hot = -1
        pieMenu.pieOpen = false
        if (typeof widget !== "undefined" && widget)
            widget.setUiOverlay(pie, false)
    }

    function run(i) {
        if (i < 0 || i >= entries.length || !entries[i].enabled)
            return
        const id = entries[i].id
        close()
        pieMenu.trigger(id)
    }

    function aim(mx, my) {
        const dx = mx - cx, dy = my - cy
        aimed = Math.sqrt(dx * dx + dy * dy) > ringOuter
        aimAngle = Math.atan2(dy, dx) * 180 / Math.PI
        // A button under the cursor wins; else the one nearest the direction.
        for (let i = 0; i < buttons.count; ++i) {
            const it = buttons.itemAt(i)
            if (entries[i].enabled && mx >= it.x && mx <= it.x + it.width && my >= it.y && my <= it.y + it.height) {
                hot = i
                return
            }
        }
        if (!aimed) {
            hot = -1
            return
        }
        let best = -1, bestDiff = 1e9
        for (let i = 0; i < entries.length; ++i) {
            if (!entries[i].enabled)
                continue
            const d = slotDir(entries[i].number)
            let diff = Math.abs(Math.atan2(d.y, d.x) * 180 / Math.PI - aimAngle)
            if (diff > 180)
                diff = 360 - diff
            if (diff < bestDiff) {
                bestDiff = diff
                best = i
            }
        }
        hot = best
    }

    NumberAnimation {
        id: flyOut
        target: pie
        property: "progress"
        from: 0
        to: 1
        duration: 220
        easing.type: Easing.OutQuart
    }

    Connections {
        target: pieMenu
        function onOpenRequested(x, y, t) { pie.open(x, y, t) }
        function onCloseRequested() { pie.close() }
        function onNumberPressed(n) {
            for (let i = 0; i < pie.entries.length; ++i)
                if (pie.entries[i].number === n) {
                    pie.run(i)
                    return
                }
        }
    }
    Connections {
        target: ctx
        function onEditorModeChanged() { pie.close() }
    }

    // Every press, move and wheel while open.
    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton | Qt.RightButton | Qt.MiddleButton
        cursorShape: Qt.ArrowCursor
        onPositionChanged: (mouse) => pie.aim(mouse.x, mouse.y)
        onPressed: (mouse) => {
            if (mouse.button !== Qt.LeftButton)
                pie.close()
        }
        onClicked: (mouse) => {
            if (mouse.button !== Qt.LeftButton)
                return
            pie.aim(mouse.x, mouse.y)
            if (pie.hot >= 0)
                pie.run(pie.hot)
            else
                pie.close()
        }
        onWheel: (wheel) => { wheel.accepted = true }
    }

    // The centre: a ring with an arc pointing at the cursor, the title above.
    Shape {
        x: pie.cx - pie.ringOuter
        y: pie.cy - pie.ringOuter
        width: pie.ringOuter * 2
        height: pie.ringOuter * 2
        opacity: pie.progress
        preferredRendererType: Shape.CurveRenderer
        ShapePath {
            strokeColor: "#10141f"
            strokeWidth: pie.ringWidth
            fillColor: "transparent"
            PathAngleArc {
                centerX: pie.ringOuter
                centerY: pie.ringOuter
                radiusX: pie.ringOuter - pie.ringWidth / 2
                radiusY: pie.ringOuter - pie.ringWidth / 2
                startAngle: 0
                sweepAngle: 360
            }
        }
        ShapePath {
            strokeColor: pie.aimed ? "#4866C5" : "transparent"
            strokeWidth: pie.ringWidth
            fillColor: "transparent"
            capStyle: ShapePath.FlatCap
            PathAngleArc {
                centerX: pie.ringOuter
                centerY: pie.ringOuter
                radiusX: pie.ringOuter - pie.ringWidth / 2
                radiusY: pie.ringOuter - pie.ringWidth / 2
                startAngle: pie.aimAngle - 25
                sweepAngle: 50
            }
        }
    }
    Text {
        id: titleLabel
        text: pie.title
        color: "#8f9dd0"
        opacity: pie.progress
        font.pixelSize: Math.round(15 * pie.tf)
        x: pie.cx - width / 2
        y: pie.cy - pie.ringOuter - 6 * pie.sf - height
    }

    Repeater {
        id: buttons
        model: pie.entries
        delegate: Rectangle {
            id: button
            required property var modelData
            required property int index
            readonly property bool lit: pie.hot === index
            readonly property point rel: pie.relPos(pie.slotDir(modelData.number), width, height)

            width: content.implicitWidth + 20 * pie.sf
            height: pie.buttonHeight
            // From the centre out to the ring.
            x: pie.cx + (rel.x + width / 2) * pie.progress - width / 2
            y: pie.cy + (rel.y + height / 2) * pie.progress - height / 2
            opacity: pie.progress * (modelData.enabled ? 1.0 : 0.4)
            radius: 4 * pie.sf
            color: lit ? "#34407a" : "#171c2b"
            border.color: lit ? "#5566aa" : "#2a3350"
            border.width: 1

            Row {
                id: content
                x: 10 * pie.sf
                anchors.verticalCenter: parent.verticalCenter
                spacing: 8 * pie.sf
                Image {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: button.modelData.icon !== ""
                    source: button.modelData.icon
                    width: 18 * pie.sf
                    height: 18 * pie.sf
                    sourceSize: Qt.size(width * 2, height * 2)
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: button.modelData.label
                    color: "#e6e9f2"
                    font.pixelSize: Math.round(15 * pie.tf)
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    leftPadding: 6 * pie.sf
                    text: button.modelData.number
                    color: "#8a92a8"
                    font.pixelSize: Math.round(15 * pie.tf)
                }
            }
        }
    }
}
