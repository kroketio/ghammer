import QtQuick

// Orientation gizmo: the world axes as the camera sees them; clicking an axis end looks along it.
Item {
    id: root
    readonly property real sf: ctx ? ctx.scaleFactor : 1.0
    width: 92 * sf
    height: 92 * sf
    property real yaw: engine3D ? engine3D.cameraYaw : -90
    property real pitch: engine3D ? engine3D.cameraPitch : 0

    PanelInputBlocker { name: "view gizmo" }

    // Camera basis (matches eng::Camera): forward f, right r, up u.
    function basis() {
        var y = yaw * Math.PI / 180, p = pitch * Math.PI / 180
        var f = [Math.cos(y) * Math.cos(p), Math.sin(p), -Math.sin(y) * Math.cos(p)]
        var r = [f[1] * 0 - f[2] * 1, f[2] * 0 - f[0] * 0, f[0] * 1 - f[1] * 0] // cross(f, up)
        var rl = Math.sqrt(r[0] * r[0] + r[2] * r[2]) || 1
        r = [r[0] / rl, 0, r[2] / rl]
        var u = [r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]]
        return { f: f, r: r, u: u }
    }
    function project(axis, sign) {
        var b = basis()
        var v = [0, 0, 0]; v[axis] = sign
        var x = v[0] * b.r[0] + v[1] * b.r[1] + v[2] * b.r[2]
        var y = v[0] * b.u[0] + v[1] * b.u[1] + v[2] * b.u[2]
        var z = v[0] * b.f[0] + v[1] * b.f[1] + v[2] * b.f[2]
        return { x: width / 2 + x * 32 * sf, y: height / 2 - y * 32 * sf, depth: z }
    }

    onYawChanged: canvas.requestPaint()
    onPitchChanged: canvas.requestPaint()
    onSfChanged: canvas.requestPaint()

    Rectangle {
        anchors.fill: parent
        radius: width / 2
        color: area.containsMouse ? "#40202838" : "#28202838"
        border.color: "#30ffffff"
    }

    readonly property var colors: ["#f24747", "#59d94d", "#4d8cff"]
    readonly property var names: ["X", "Y", "Z"]

    Canvas {
        id: canvas
        anchors.fill: parent
        onPaint: {
            var ctx2 = getContext("2d")
            ctx2.reset()
            var items = []
            for (var a = 0; a < 3; ++a)
                for (var s = -1; s <= 1; s += 2)
                    items.push({ a: a, s: s, p: root.project(a, s) })
            items.sort(function(l, r) { return r.p.depth - l.p.depth })
            for (var i = 0; i < items.length; ++i) {
                var it = items[i]
                var col = root.colors[it.a]
                ctx2.globalAlpha = it.s > 0 ? 1.0 : 0.45
                if (it.s > 0) {
                    ctx2.strokeStyle = col
                    ctx2.lineWidth = 2.5 * root.sf
                    ctx2.beginPath()
                    ctx2.moveTo(width / 2, height / 2)
                    ctx2.lineTo(it.p.x, it.p.y)
                    ctx2.stroke()
                }
                ctx2.fillStyle = col
                ctx2.beginPath()
                ctx2.arc(it.p.x, it.p.y, (it.s > 0 ? 9 : 6) * root.sf, 0, 2 * Math.PI)
                ctx2.fill()
                if (it.s > 0) {
                    ctx2.fillStyle = "#101420"
                    ctx2.font = "bold " + Math.round(11 * root.sf) + "px sans-serif"
                    ctx2.textAlign = "center"
                    ctx2.textBaseline = "middle"
                    ctx2.fillText(root.names[it.a], it.p.x, it.p.y + 0.5)
                }
            }
            ctx2.globalAlpha = 1.0
        }
    }

    MouseArea {
        id: area
        anchors.fill: parent
        hoverEnabled: true
        onClicked: function(m) {
            var best = -1, bestD = 14 * root.sf, bestS = 1
            for (var a = 0; a < 3; ++a)
                for (var s = -1; s <= 1; s += 2) {
                    var p = root.project(a, s)
                    var d = Math.hypot(p.x - m.x, p.y - m.y)
                    if (d < bestD) { bestD = d; best = a; bestS = s }
                }
            // Clicking +X means "look from +X", i.e. along -X.
            if (best >= 0 && engine3D)
                engine3D.lookAlongAxis(best, -bestS)
        }
    }
}
