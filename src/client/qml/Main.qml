import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import RhiView 1.0 as RV

import Main 1.0
import GlobalEnums 1.0

Rectangle {
    id: root
    anchors.fill: parent
    color: "#232a4e"

    readonly property int fullscreen: 0
    readonly property int quad: 1
    readonly property int split21: 2

    property int layoutMode: root.fullscreen
    property int splitterWidth: 10

    // The 3D view's overlays (not its menu bar) follow the UI scale; their text UI × font scale.
    readonly property real sf: ctx ? ctx.scaleFactor : 1.0
    readonly property real tf: ctx && ctx.textScaleFactor ? ctx.textScaleFactor : sf
    // The 3D view's menu bar grows at half the rate above 1× (2× -> 1.5): full size it crowds the view.
    readonly property real barScale: 1.0 + (Math.max(sf, tf) - 1.0) * 0.5

    focus: true

    Pane {
        anchors.fill: parent
        focusPolicy: Qt.ClickFocus
    }

    // The bar's menus are C++ widgets, popped up at the button's bottom-left corner.
    function openMenu(name, button) {
        const at = button.mapToItem(root, 0, button.height)
        window.showViewportMenu(name, at.x, at.y)
    }

    // Every editor mode keeps its own layout; the first visit uses the default
    // (Object quad, Material single; the vertex tool keeps the current one).
    property int currentEditorMode: Editor.OBJECT
    property var modeLayouts: ({})

    // A new map resets every mode to its default layout.
    function resetModeLayouts() {
        modeLayouts = ({})
    }

    function switchMode(mode) {
        if (mode === currentEditorMode)
            return;
        modeLayouts[currentEditorMode] = layoutMode;
        currentEditorMode = mode;
        let next = modeLayouts[mode];
        if (next === undefined) {
            if (mode == Editor.OBJECT)
                next = root.quad
            else if (mode == Editor.MATERIAL)
                next = root.fullscreen
            else
                next = layoutMode
        }
        // Through MainWindow so the layout actions (toolbar, F toggle) stay in sync.
        window.setViewportLayout(next)
    }

    Connections {
        target: ctx
        function onEditorModeChanged(mode) { root.switchMode(mode); }
    }

    // Layout is driven by MainWindow actions, not by keys here.
    function setLayout(mode) {
        if (root.layoutMode !== mode)
            root.layoutMode = mode
    }

    Rectangle {
        id: grid
        focus: true
        anchors.fill: parent
        color: "transparent"

        property real vRatio: 0.5
        property real hRatio: 0.5

        readonly property real vSplit: width * vRatio
        readonly property real hSplit: height * hRatio

        // Top-left
        Rectangle {
            id: viewportTopLeft
            x: 0
            y: 0
            width: root.layoutMode === root.fullscreen ? grid.width :
                    root.layoutMode === root.split21 ? grid.vSplit : grid.vSplit
            height: root.layoutMode === root.quad ? grid.hSplit : grid.height
            color: "transparent"

            // The 3D view's menu bar: UI scale, but big enough for its text at the text scale.
            property real menuHeight: Math.round(40 * root.barScale)

            onWidthChanged: widget.viewportWidth = width;
            onHeightChanged: widget.viewportHeight = height;

            Component.onCompleted: {
                widget.viewportWidth = width;
                widget.viewportHeight = height;
            }

            // camera speed indicator
            Rectangle {
                visible: widget.showCameraSpeedChangedIndicator
                z: view3d.z+1
                anchors.left: parent.left
                anchors.leftMargin: 32 * root.sf
                anchors.topMargin: 32 * root.sf + viewportTopLeft.menuHeight
                anchors.bottomMargin: 32 * root.sf
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                radius: 4 * root.sf

                width: 12 * root.sf
                color: "#4D000000"

                Rectangle {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    height: parent.height * widget.cameraSpeedPct / 100
                    color: "#4D92B6"
                }
            }

            Item {
                id: view3d
                anchors.fill: parent
                anchors.rightMargin: root.layoutMode !== root.fullscreen ? root.splitterWidth / 2 : 0
                anchors.bottomMargin: root.layoutMode === root.quad ? root.splitterWidth / 2 : 0

                // Entities and materials dragged in from the side panels.
                DropArea {
                    anchors.fill: parent
                    keys: ["application/x-editor-entity", "application/x-editor-material", "application/x-editor-model"]
                    onDropped: function(drop) {
                        var p = mapToItem(root, drop.x, drop.y)
                        if (drop.hasFormat("application/x-editor-model"))
                            window.dropOnViewport("model", drop.getDataAsString("application/x-editor-model"), p.x, p.y)
                        else if (drop.hasFormat("application/x-editor-entity"))
                            window.dropOnViewport("entity", drop.getDataAsString("application/x-editor-entity"), p.x, p.y)
                        else
                            window.dropOnViewport("material", drop.getDataAsString("application/x-editor-material"), p.x, p.y)
                        drop.acceptProposedAction()
                    }
                }

                RV.Viewport3D {
                    id: fbo
                    anchors.fill: parent
                    msaaSamples: engine3D ? engine3D.msaaSamples : 2
                }

                Rectangle {
                    height: viewportTopLeft.menuHeight
                    width: parent.width
                    color: "#4D000000"
                    z: fbo.z + 1

                    PanelInputBlocker { name: "viewport menu bar" }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 8 * root.barScale
                        spacing: 10 * root.barScale

                        Item {
                            Layout.preferredWidth: 8 * root.barScale
                            Layout.fillHeight: true
                        }

                        // Editor mode: black Object, orange Edit, green Material.
                        Rectangle {
                            readonly property int mode: ctx ? ctx.editorMode : 0
                            color: mode === 2 ? "#3fc25a" : mode === 1 ? "#f08c28" : "black"
                            border.color: "#c8cfdd"
                            border.width: 1
                            Layout.preferredHeight: 16 * root.barScale
                            Layout.preferredWidth: 16 * root.barScale
                            Layout.alignment: Qt.AlignVCenter
                            radius: 16 * root.barScale
                        }

                        Item {
                            Layout.preferredWidth: 4 * root.barScale
                            Layout.fillHeight: true
                        }

                        ViewportMenuBarTextButton {
                            id: viewButton
                            text: "View"
                            tf: root.barScale
                            Layout.preferredHeight: Math.round(26 * root.barScale)
                            Layout.preferredWidth: textItem.implicitWidth + 22 * root.barScale
                            onClicked: root.openMenu("view", viewButton)
                        }

                        ViewportMenuBarTextButton {
                            id: selectButton
                            text: "Select"
                            tf: root.barScale
                            Layout.preferredHeight: Math.round(26 * root.barScale)
                            Layout.preferredWidth: textItem.implicitWidth + 22 * root.barScale
                            onClicked: root.openMenu("select", selectButton)
                        }

                        ViewportMenuBarTextButton {
                            id: addButton
                            text: "Add"
                            tf: root.barScale
                            Layout.preferredHeight: Math.round(26 * root.barScale)
                            Layout.preferredWidth: textItem.implicitWidth + 22 * root.barScale
                            onClicked: root.openMenu("add", addButton)
                        }

                        ViewportMenuBarTextButton {
                            id: objectButton
                            text: "Object"
                            tf: root.barScale
                            Layout.preferredHeight: Math.round(26 * root.barScale)
                            Layout.preferredWidth: textItem.implicitWidth + 22 * root.barScale
                            onClicked: root.openMenu("object", objectButton)
                        }

                        ViewportMenuBarToggleButton {
                            text: "TL"
                            tip: qsTr("Texture lock") + (ctx ? ctx.shortcutSuffix("brush.textureLock", ctx.shortcutRevision) : "")
                            checked: engine3D ? engine3D.textureLock : false
                            tf: root.barScale
                            Layout.preferredWidth: Math.round(28 * root.barScale)
                            Layout.preferredHeight: Math.round(28 * root.barScale)
                            Layout.alignment: Qt.AlignVCenter
                            onClicked: {
                                if (engine3D)
                                    engine3D.textureLock = !engine3D.textureLock
                            }
                        }

                        ViewportMenuBarToggleButton {
                            text: "TS"
                            tip: qsTr("Texture scale lock") + (ctx ? ctx.shortcutSuffix("brush.scaleLock", ctx.shortcutRevision) : "")
                            checked: engine3D ? engine3D.textureScaleLock : false
                            tf: root.barScale
                            Layout.preferredWidth: Math.round(28 * root.barScale)
                            Layout.preferredHeight: Math.round(28 * root.barScale)
                            Layout.alignment: Qt.AlignVCenter
                            onClicked: {
                                if (engine3D)
                                    engine3D.textureScaleLock = !engine3D.textureScaleLock
                            }
                        }

                        // Generated brushes: modifier output, or (off) the source brushes.
                        ViewportMenuBarToggleButton {
                            text: "GB"
                            tip: qsTr("Show generated brushes") + (ctx ? ctx.shortcutSuffix("view.showGenerated", ctx.shortcutRevision) : "")
                            checked: engine3D ? engine3D.showModOutput : true
                            tf: root.barScale
                            Layout.preferredWidth: Math.round(28 * root.barScale)
                            Layout.preferredHeight: Math.round(28 * root.barScale)
                            Layout.alignment: Qt.AlignVCenter
                            onClicked: {
                                if (engine3D)
                                    engine3D.showModOutput = !engine3D.showModOutput
                            }
                        }

                        Item {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                        }
                    }
                }

                // Every viewport message (ui::ViewportNotices), stacked in the lower-left corner.
                Column {
                    z: fbo.z + 3
                    anchors.left: parent.left
                    anchors.bottom: parent.bottom
                    // clear of the camera speed bar while it shows
                    anchors.leftMargin: (widget.showCameraSpeedChangedIndicator ? 56 : 12) * root.sf
                    anchors.bottomMargin: 12 * root.sf
                    spacing: 5 * root.sf
                    Repeater {
                        model: viewportNotices
                        NoticeBox {
                            text: model.text
                            fontPx: 12
                        }
                    }
                }

                ViewGizmo {
                    z: fbo.z + 3
                    visible: prefShowViewGizmo
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    anchors.rightMargin: 28 * root.sf
                    anchors.bottomMargin: 28 * root.sf
                }

                // Camera3D entity selected: look through it.
                Rectangle {
                    id: cameraPreviewBox
                    z: fbo.z + 2
                    visible: engine3D && (engine3D.cameraPreviewAvailable || engine3D.cameraPreview)
                    anchors.left: parent.left
                    anchors.leftMargin: 10 * root.sf
                    anchors.top: parent.top
                    anchors.topMargin: viewportTopLeft.menuHeight + 10 * root.sf
                    width: previewCheck.implicitWidth + 24 * root.sf
                    height: previewCheck.implicitHeight + 16 * root.sf
                    radius: 5 * root.sf
                    color: engine3D && engine3D.cameraPreview ? "#3a2a30" : "#26304a"
                    border.color: engine3D && engine3D.cameraPreview ? "#d98a95" : "#4a5a7a"
                    border.width: 1

                    PanelInputBlocker { name: "camera preview" }

                    QCheckBox {
                        id: previewCheck
                        anchors.centerIn: parent
                        text: "Preview"
                        fontPx: 12
                        checked: engine3D ? engine3D.cameraPreview : false
                        onToggled: {
                            if (engine3D)
                                engine3D.cameraPreview = checked
                            // toggle() replaced the binding; follow the engine again
                            checked = Qt.binding(function() { return engine3D ? engine3D.cameraPreview : false })
                        }
                    }
                }

                Rectangle {
                    id: renderModeButton
                    z: fbo.z + 2
                    anchors.right: parent.right
                    anchors.rightMargin: 10 * root.sf
                    anchors.top: parent.top
                    anchors.topMargin: viewportTopLeft.menuHeight + 10 * root.sf
                    width: renderModeText.implicitWidth + 12 * root.sf
                    height: renderModeText.implicitHeight + 5 * root.sf
                    radius: 3 * root.sf
                    color: renderModeMouse.containsMouse ? "#3a4a68" : "#26304a"
                    border.color: "#4a5a7a"
                    border.width: 1

                    PanelInputBlocker { name: "render mode button" }

                    Text {
                        id: renderModeText
                        anchors.centerIn: parent
                        color: "#dfe6f2"
                        font.pixelSize: Math.round(10 * root.tf)
                        font.bold: true
                        text: engine3D && engine3D.renderMode === 1 ? "Lit" : "Fullbright"
                    }

                    MouseArea {
                        id: renderModeMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            if (!engine3D)
                                return
                            engine3D.renderMode = engine3D.renderMode === 1 ? 0 : 1
                        }
                    }
                }

            }
        }

        // Top-right: top view
        ViewportGrid2D {
            visible: root.layoutMode !== root.fullscreen
            x: grid.vSplit
            y: 0
            width: grid.width - grid.vSplit
            height: grid.hSplit
            title: "Top"
            perspective: RV.GridView.Top
        }

        // Bottom-left: front view
        ViewportGrid2D {
            visible: root.layoutMode !== root.fullscreen
            x: root.layoutMode === root.split21 ? grid.vSplit : 0
            y: grid.hSplit
            width: root.layoutMode === root.split21 ? grid.width - grid.vSplit : grid.vSplit
            height: grid.height - grid.hSplit
            title: "Front"
            perspective: RV.GridView.Front
        }

        // Bottom-right: side view
        ViewportGrid2D {
            visible: root.layoutMode === root.quad
            x: grid.vSplit
            y: grid.hSplit
            width: grid.width - grid.vSplit
            height: grid.height - grid.hSplit
            title: "Side"
            perspective: RV.GridView.Side
        }


        // Vertical splitter
        Rectangle {
            visible: root.layoutMode !== root.fullscreen
            x: grid.vSplit - root.splitterWidth / 2
            y: 0
            width: root.splitterWidth
            height: root.layoutMode === root.split21 ? grid.height : grid.height
            color: root.color

            MouseArea {
                anchors.fill: parent
                anchors.margins: -14
                cursorShape: Qt.SizeHorCursor
                drag.target: parent
                drag.axis: Drag.XAxis
                onPositionChanged: {
                    var x = Math.max(50, Math.min(grid.width - 50, parent.x + root.splitterWidth / 2))
                    grid.vRatio = x / grid.width
                }
            }
        }

        // Horizontal splitter
        Rectangle {
            visible: root.layoutMode !== root.fullscreen
            x: root.layoutMode === root.split21 ? grid.vSplit : 0
            y: grid.hSplit - root.splitterWidth / 2
            width: root.layoutMode === root.split21 ? grid.width - grid.vSplit : grid.width
            height: root.splitterWidth
            color: root.color

            MouseArea {
                anchors.fill: parent
                anchors.margins: -14
                cursorShape: Qt.SizeVerCursor
                drag.target: parent
                drag.axis: Drag.YAxis
                onPositionChanged: {
                    var y = Math.max(50, Math.min(grid.height - 50, parent.y + root.splitterWidth / 2))
                    grid.hRatio = y / grid.height
                }
            }
        }

        // Center XY handle (only for Quad)
        Item {
            visible: root.layoutMode === root.quad
            width: 20
            height: 20
            x: grid.vSplit - width / 2
            y: grid.hSplit - height / 2

            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.SizeAllCursor
                drag.target: parent
                drag.axis: Drag.XAndYAxis
                onPositionChanged: {
                    var x = Math.max(50, Math.min(grid.width - 50, parent.x + width / 2))
                    var y = Math.max(50, Math.min(grid.height - 50, parent.y + height / 2))
                    grid.vRatio = x / grid.width
                    grid.hRatio = y / grid.height
                }
            }
        }
    }


    // Pie menu and Quick Material strip, above everything (they open at the cursor, in root coordinates).
    PieMenu {
        z: 10000
    }
    QuickMaterialStrip {
        z: 10001
    }
}
