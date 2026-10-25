import QtQuick

// Horizontal pan bar. Dragging moves it relative to where the finger went down; double tap
// centres it. While a finger is on it the strip list stops scrolling, so the drag is not stolen.
Item {
    id: root
    property real value: 0 // -1 .. 1
    property Item flick: null
    signal moved(real value)

    height: 36

    Rectangle {
        id: track
        anchors { left: parent.left; right: parent.right; verticalCenter: parent.verticalCenter }
        height: 22
        radius: 4
        color: Theme.control
        // Filled part from the centre to the value
        Rectangle {
            x: Math.min(parent.width / 2, parent.width / 2 * (1 + root.value))
            width: Math.abs(root.value) * parent.width / 2
            height: parent.height
            radius: 4
            color: point.active ? Theme.accent : "#6f7883"
        }
        // Centre marks above and below the label, so that they do not cross it.
        Rectangle { x: parent.width / 2 - 1; width: 2; height: 4; color: "#888" }
        Rectangle { x: parent.width / 2 - 1; y: parent.height - 4; width: 2; height: 4; color: "#888" }
    }
    Text {
        anchors.centerIn: track
        color: "white"
        font.pixelSize: 11
        text: Math.abs(root.value) < 0.005 ? "C" : (root.value < 0 ? "L" : "R") + Math.round(Math.abs(root.value) * 100)
    }

    PointHandler {
        id: point
        onActiveChanged: if (root.flick) root.flick.interactive = !active
    }
    DragHandler {
        id: drag
        target: null
        yAxis.enabled: false
        grabPermissions: PointerHandler.CanTakeOverFromAnything
        property real startValue: 0
        onActiveChanged: if (active) startValue = root.value
        onTranslationChanged: {
            if (!active)
                return
            root.moved(Math.max(-1, Math.min(1, startValue + translation.x / Math.max(1, track.width / 2))))
        }
    }
    TapHandler {
        onTapped: if (tapCount === 2) root.moved(0)
    }
}
