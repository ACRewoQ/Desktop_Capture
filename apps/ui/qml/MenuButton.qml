import QtQuick

Rectangle {
    id: root
    property string label
    property bool active: true
    signal tapped()

    height: 52
    radius: Theme.radius
    color: tap.pressed ? Theme.controlPressed : Theme.control
    opacity: active ? 1 : 0.4
    Text {
        anchors.centerIn: parent
        color: "white"
        font.pixelSize: 16
        text: root.label
    }
    TapHandler { id: tap; enabled: root.active; onTapped: root.tapped() }
}
