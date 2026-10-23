import QtQuick

Rectangle {
    id: root
    property string label
    property bool on: false
    property color onColor: Theme.accent
    property color onLabelColor: "white"
    signal tapped()

    height: Theme.touch
    radius: Theme.radius
    color: on ? onColor : tap.pressed ? Theme.controlPressed : Theme.control
    Text {
        anchors.centerIn: parent
        text: root.label
        color: root.on ? root.onLabelColor : "white"
        font.pixelSize: 16
        font.bold: true
    }
    TapHandler { id: tap; onTapped: root.tapped() }
}
