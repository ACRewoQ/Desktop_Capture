import QtQuick

// The "+" at the end of the strips: adds a stereo or a mono track (up to 64).
Item {
    width: Theme.stripWidth
    opacity: tracks.canAdd ? 1 : 0.4

    Column {
        anchors.centerIn: parent
        spacing: 12
        Repeater {
            model: [{ label: "+ ステレオ", channels: 2 }, { label: "+ モノ", channels: 1 }]
            Rectangle {
                required property var modelData
                objectName: "addTrack_" + modelData.channels
                width: Theme.stripWidth - 16
                height: 64
                radius: Theme.radius
                color: tap.pressed ? Theme.controlPressed : Theme.control
                border.color: "#555b65"
                Text {
                    anchors.centerIn: parent
                    color: Theme.text
                    font.pixelSize: 15
                    text: parent.modelData.label
                }
                TapHandler {
                    id: tap
                    enabled: tracks.canAdd
                    onTapped: tracks.addTrack(parent.modelData.channels)
                }
            }
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            color: Theme.subText
            font.pixelSize: 12
            text: tracks.count + " / " + tracks.maxTracks
        }
    }
}
