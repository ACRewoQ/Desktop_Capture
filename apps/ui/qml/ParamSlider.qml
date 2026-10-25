import QtQuick
import DcUi.Params

// Horizontal slider for a number parameter. The finger moves the value relative to where it went
// down (no jump), logarithmic when log is set; a double tap goes back to the default.
Item {
    id: root
    property alias path: param.path
    property real from: 0
    property real to: 1
    property bool log: false
    property int steps: 0          // 0 = continuous, n >= 2 = n positions
    property var defaultValue: undefined
    readonly property alias param: param
    readonly property bool held: drag.active
    signal released()

    function toPos(v) {
        if (to === from) return 0
        if (log && from > 0) return (Math.log(v) - Math.log(from)) / (Math.log(to) - Math.log(from))
        return (v - from) / (to - from)
    }
    function toValue(p) {
        p = Math.max(0, Math.min(1, p))
        if (steps >= 2) p = Math.round(p * (steps - 1)) / (steps - 1)
        if (log && from > 0) return Math.exp(Math.log(from) + p * (Math.log(to) - Math.log(from)))
        return from + p * (to - from)
    }

    width: parent ? parent.width : 0
    height: Theme.touch
    Param { id: param }
    readonly property real position: Math.max(0, Math.min(1, toPos(Number(param.value))))

    Rectangle {
        id: groove
        anchors { left: parent.left; right: parent.right; verticalCenter: parent.verticalCenter; leftMargin: 14; rightMargin: 14 }
        height: 6
        radius: 3
        color: Theme.control
        Rectangle {
            width: root.position * parent.width
            height: parent.height
            radius: 3
            color: root.held ? Theme.accent : Theme.accentDim
        }
    }
    Rectangle {
        x: groove.x + root.position * groove.width - width / 2
        anchors.verticalCenter: parent.verticalCenter
        width: 28
        height: 28
        radius: 14
        color: root.held ? Theme.accent : "#d0d4da"
    }

    DragHandler {
        id: drag
        target: null
        yAxis.enabled: false
        property real startPos: 0
        onActiveChanged: {
            if (active) startPos = root.position
            else root.released()
        }
        onTranslationChanged: if (active) param.set(root.toValue(startPos + translation.x / Math.max(1, groove.width)))
    }
    TapHandler {
        onTapped: if (tapCount === 2 && root.defaultValue !== undefined) { param.set(root.defaultValue); root.released() }
    }
}
