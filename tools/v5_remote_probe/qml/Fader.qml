import QtQuick

// Touch fader built from plain items. Dragging anywhere on it moves the value relative to where the
// finger went down (no jump), so several fingers can each hold one fader. Double tap resets to 0 dB.
Item {
    id: root

    property int row: -1
    property real position: 0          // 0..1, from the model
    property real zeroDbPosition: 0.833
    readonly property bool held: drag.active

    signal moved(real position)
    signal grabChanged(bool grabbed)

    objectName: "fader_" + row

    Rectangle {
        anchors.horizontalCenter: parent.horizontalCenter
        y: thumb.height / 2
        width: 6
        height: root.height - thumb.height
        radius: 3
        color: "#3a3f47"
    }

    Rectangle {
        id: thumb
        objectName: "faderThumb_" + root.row
        width: root.width * 0.8
        height: 44
        radius: 6
        x: (root.width - width) / 2
        y: (1 - root.position) * (root.height - height)
        color: root.held ? "#4fc3f7" : "#d0d4da"
        Rectangle { anchors.centerIn: parent; width: parent.width * 0.7; height: 2; color: "#555" }
    }

    DragHandler {
        id: drag
        target: null
        xAxis.enabled: false
        property real startPosition: 0
        onActiveChanged: {
            if (active)
                startPosition = root.position
            root.grabChanged(active)
        }
        onTranslationChanged: {
            if (!active)
                return
            const travel = Math.max(1, root.height - thumb.height)
            root.moved(Math.max(0, Math.min(1, startPosition - translation.y / travel)))
        }
    }

    TapHandler {
        onTapped: if (tapCount === 2) root.moved(root.zeroDbPosition)
    }
}
