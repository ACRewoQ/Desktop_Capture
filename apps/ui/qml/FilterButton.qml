import QtQuick

// Low cut / high cut: tap switches it on and off, a vertical drag changes the frequency
// (logarithmic, up = higher). Vertical so that a sideways swipe still scrolls the strips.
Rectangle {
    id: root
    property string label
    property bool on: false
    property real hz: 1000
    property real minHz: 20
    property real maxHz: 20000
    signal toggled()
    signal hzMoved(real hz)

    height: Theme.touch
    radius: Theme.radius
    color: on ? Theme.filter : drag.active || tap.pressed ? Theme.controlPressed : Theme.control
    opacity: on ? 1 : 0.85

    Column {
        anchors.centerIn: parent
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: root.label
            color: "white"
            font.pixelSize: 12
            font.bold: true
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: tracks.hzText(root.hz)
            color: root.on ? "white" : Theme.subText
            font.pixelSize: 12
        }
    }

    TapHandler { id: tap; onTapped: root.toggled() }
    DragHandler {
        id: drag
        target: null
        xAxis.enabled: false
        property real startLog: 0
        onActiveChanged: if (active) startLog = Math.log(root.hz)
        onTranslationChanged: {
            if (!active)
                return
            // 240 px of travel covers the whole range.
            const span = Math.log(root.maxHz) - Math.log(root.minHz)
            const v = Math.exp(startLog - translation.y / 240 * span)
            root.hzMoved(Math.max(root.minHz, Math.min(root.maxHz, v)))
        }
    }
}
