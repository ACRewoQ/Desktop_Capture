import QtQuick

// Connection state, track count and the recording button.
Rectangle {
    id: bar
    height: 60
    color: Theme.panel

    Row {
        id: left
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

        // Recording settings and file; shortened (or hidden) when the window is narrow.
        Column {
            anchors.verticalCenter: parent.verticalCenter
            width: Math.max(0, Math.min(Math.max(summaryText.implicitWidth, Math.min(fileText.implicitWidth, 420)),
                                        bar.width - left.width - recordButton.width - 60))
            visible: width > 80
            Text {
                id: summaryText
                anchors.right: parent.right
                width: Math.min(implicitWidth, parent.width)
                elide: Text.ElideRight
                color: Theme.subText
                font.pixelSize: 12
                text: recorder.summary
            }
            Text {
                id: fileText
                anchors.right: parent.right
                width: Math.min(implicitWidth, 420, parent.width)
                elide: Text.ElideMiddle
                color: recorder.error ? Theme.mute : Theme.subText
                font.pixelSize: 12
                text: recorder.error ? recorder.error : recorder.file
            }
        }
        RecordButton { id: recordButton; anchors.verticalCenter: parent.verticalCenter }
    }
}
