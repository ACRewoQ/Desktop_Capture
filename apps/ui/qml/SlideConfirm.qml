import QtQuick

// For actions that cannot be undone: the knob has to be slid to the right end. Letting go earlier
// moves it back.
Rectangle {
    id: root
    property string label: "スライドして確定"
    property color tint: Theme.mute
    signal confirmed()

    height: 52
    radius: height / 2
    color: Theme.control
    clip: true

    Rectangle {
        anchors { left: parent.left; top: parent.top; bottom: parent.bottom }
        width: knob.x + knob.width
        radius: root.radius
        color: root.tint
        opacity: 0.35 + 0.65 * knob.x / Math.max(1, root.width - knob.width)
    }
    Text {
        anchors.centerIn: parent
        color: Theme.text
        font.pixelSize: 15
        text: root.label
    }
    Rectangle {
        id: knob
        objectName: "slideKnob"
        width: root.height - 8
        height: width
        radius: width / 2
        y: 4
        x: 4
        color: "white"
        Text { anchors.centerIn: parent; text: "»"; font.pixelSize: 22; color: root.tint }
        Behavior on x { enabled: !drag.active; NumberAnimation { duration: 150 } }
    }
    DragHandler {
        id: drag
        target: null
        yAxis.enabled: false
        property real startX: 4
        onActiveChanged: {
            if (active) {
                startX = knob.x
                return
            }
            if (knob.x >= root.width - knob.width - 8) root.confirmed()
            knob.x = 4
        }
        onTranslationChanged: {
            if (active) knob.x = Math.max(4, Math.min(root.width - knob.width - 4, startX + translation.x))
        }
    }
}
