import QtQuick

// The QML layer's claim on the cursor: whether it is inside the root item (nested controls included).
// The hosting widget checks it before the viewport gets a press or hover; live geometry, so it never goes stale.
HoverHandler {
    id: blocker

    property string name: "panel"

    onHoveredChanged: {
        if (typeof widget !== "undefined" && widget)
            widget.setUiOverlay(blocker, blocker.hovered)
    }
}
