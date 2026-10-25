import QtQuick

Rectangle {
    id: root
    property string label
    property bool on: false
    property color onColor: Theme.accent
    property color onLabelColor: "white"
    property int fontSize: 16
    signal tapped()

    height: Theme.touch
    radius: Theme.radius
    color: on ? onColor : tap.pressed ? Theme.controlPressed : Theme.control
    Text {
        anchors.centerIn: parent
        text: root.label
        color: root.on ? root.onLabelColor : "white"
        font.pixelSize: root.fontSize
        font.bold: true
    }
    TapHandler { id: tap; onTapped: root.tapped() }
}
