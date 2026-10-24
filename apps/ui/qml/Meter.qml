import QtQuick

// Peak meter, one bar per channel (scale -60 .. +6 dB). The red cap shows a clip since the last tap.
Item {
    id: root
    property bool stereo: true
    property real levelL: 0
    property real levelR: 0
    property bool clip: false
    signal clipCleared()

    function colorOf(l) { return l > 0.909 ? "#ef5350" : l > 0.788 ? Theme.warn : Theme.ok } // 0 dB, -8 dB

    Rectangle {
        id: clipCap
        anchors { top: parent.top; left: parent.left; right: parent.right }
        height: 8
        radius: 2
        color: root.clip ? "#ef5350" : "#2a2d33"
        TapHandler { margin: 10; onTapped: root.clipCleared() }
    }
    Row {
        anchors { top: clipCap.bottom; topMargin: 2; bottom: parent.bottom; left: parent.left; right: parent.right }
        spacing: 2
        Repeater {
            model: root.stereo ? 2 : 1
            Rectangle {
                required property int index
                readonly property real level: index === 0 ? root.levelL : root.levelR
                width: root.stereo ? (root.width - 2) / 2 : root.width
                height: parent.height
                color: "#111317"
                Rectangle {
                    anchors.bottom: parent.bottom
                    width: parent.width
                    height: parent.height * parent.level
                    color: root.colorOf(parent.level)
                }
            }
        }
    }
}
