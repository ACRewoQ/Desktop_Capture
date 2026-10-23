import QtQuick

// Connection state, track count and the recording button.
Rectangle {
    height: 60
    color: Theme.panel

    Row {
        anchors { left: parent.left; leftMargin: 14; verticalCenter: parent.verticalCenter }
        spacing: 10

        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            width: 12; height: 12; radius: 6
            color: connection.ready ? Theme.ok : Theme.warn
        }
        Column {
            anchors.verticalCenter: parent.verticalCenter
            Text {
                color: Theme.text
                font.pixelSize: 15
                text: connection.ready ? (connection.serverName || "エンジン") : connection.statusText
            }
            Text {
                color: Theme.subText
                font.pixelSize: 12
                text: connection.address + "　トラック " + tracks.count + " / " + tracks.maxTracks
            }
        }
    }

    Row {
        anchors { right: parent.right; rightMargin: 12; verticalCenter: parent.verticalCenter }
        spacing: 12

        Column {
            anchors.verticalCenter: parent.verticalCenter
            Text {
                anchors.right: parent.right
                color: Theme.subText
                font.pixelSize: 12
                text: recorder.summary
            }
            Text {
                anchors.right: parent.right
                width: Math.min(implicitWidth, 420)
                elide: Text.ElideMiddle
                color: recorder.error ? Theme.mute : Theme.subText
                font.pixelSize: 12
                text: recorder.error ? recorder.error : recorder.file
            }
        }
        RecordButton { anchors.verticalCenter: parent.verticalCenter }
    }
}
