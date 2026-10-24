import QtQuick

// Short message at the bottom (errors from the engine). Disappears after a few seconds.
Rectangle {
    id: root
    function show(message) {
        label.text = message
        opacity = 1
        timer.restart()
    }

    anchors { bottom: parent.bottom; bottomMargin: 24; horizontalCenter: parent.horizontalCenter }
    width: Math.min(label.implicitWidth + 40, parent.width - 40)
    height: label.implicitHeight + 20
    radius: 8
    color: "#e0202329"
    border.color: Theme.mute
    opacity: 0
    visible: opacity > 0
    Behavior on opacity { NumberAnimation { duration: 200 } }

    Text {
        id: label
        anchors.centerIn: parent
        width: Math.min(implicitWidth, root.parent.width - 80)
        wrapMode: Text.Wrap
        color: Theme.text
        font.pixelSize: 15
    }
    Timer { id: timer; interval: 4000; onTriggered: root.opacity = 0 }
    TapHandler { onTapped: root.opacity = 0 }
}
