import QtQuick

// Touch fader built from plain items. Dragging anywhere on it moves the value relative to where the
// finger went down (no jump), so several fingers can each hold one fader. Double tap resets to 0 dB.
// Fine mode: hold the finger still for a moment before moving, and the fader follows at a fifth of the
// speed until it is let go (the thumb turns orange).
Item {
    id: root

    property real position: 0          // 0..1, from the model
    property real zeroDbPosition: 0.833
    property real fineFactor: 0.2
    readonly property bool held: drag.active
    readonly property bool fine: drag.fine || (hold.fineArmed && hold.pressed)

    signal moved(real position)

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
        width: root.width * 0.8
        height: 44
        radius: 6
        x: (root.width - width) / 2
        y: (1 - root.position) * (root.height - height)
        color: root.fine ? Theme.warn : root.held ? "#4fc3f7" : "#d0d4da"
        Rectangle { anchors.centerIn: parent; width: parent.width * 0.7; height: 2; color: "#555" }
    }

    DragHandler {
        id: drag
        target: null
        xAxis.enabled: false
        // Two fingers on two faders are two drags, never a pinch of the strip width.
        grabPermissions: PointerHandler.CanTakeOverFromItems | PointerHandler.CanTakeOverFromHandlersOfDifferentType
        property real startPosition: 0
        property bool fine: false
        onActiveChanged: {
            if (active) {
                startPosition = root.position
                fine = hold.fineArmed
            } else {
                fine = false
            }
        }
        onTranslationChanged: {
            if (!active)
                return
            const travel = Math.max(1, root.height - thumb.height) / (fine ? root.fineFactor : 1)
            root.moved(Math.max(0, Math.min(1, startPosition - translation.y / travel)))
        }
    }

    TapHandler {
        id: hold
        property bool fineArmed: false
        longPressThreshold: 0.6
        onTapped: if (tapCount === 2) root.moved(root.zeroDbPosition)
        onLongPressed: fineArmed = true
        onPressedChanged: if (pressed) fineArmed = false
    }
}
