import QtQuick

Rectangle {
    id: root
    property string label
    property bool active: true
    property int fontSize: 16
    signal tapped()

    height: 52
    radius: Theme.radius
    color: Theme.control
    opacity: active ? 1 : 0.4
    Rectangle {
        anchors.fill: parent
        radius: parent.radius
        color: "white"
        opacity: tap.pressed ? 0.12 : 0
    }
    Text {
        anchors.centerIn: parent
        width: parent.width - 8
        horizontalAlignment: Text.AlignHCenter
        elide: Text.ElideRight
        color: "white"
        font.pixelSize: root.fontSize
        text: root.label
    }
    TapHandler { id: tap; enabled: root.active; onTapped: root.tapped() }
}
