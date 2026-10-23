import QtQuick

// Starts / stops the "デスクトップキャプチャ" output. Shows the elapsed time while recording.
Rectangle {
    id: root
    width: 168
    height: Theme.touch
    radius: Theme.radius
    color: recorder.recording ? Theme.record : tap.pressed ? Theme.controlPressed : Theme.control
    opacity: connection.ready && !recorder.busy ? 1 : 0.5
    objectName: "recordButton"

    Row {
        anchors.centerIn: parent
        spacing: 8
        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            width: 14; height: 14
            radius: recorder.recording ? 2 : 7
            color: recorder.recording ? "white" : Theme.record
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            color: "white"
            font.pixelSize: 17
            font.bold: true
            text: recorder.recording ? recorder.elapsedText : recorder.busy ? "…" : "録画"
        }
    }

    TapHandler {
        id: tap
        enabled: connection.ready && !recorder.busy
        onTapped: recorder.toggle()
    }
}
