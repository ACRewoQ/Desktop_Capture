import QtQuick
import QtQuick.Window
import DcV6

// dc-v6-preview window: the decoded test stream, an on-screen QPC clock and the live latency figures.
// A screenshot shows the clock next to the generation time burned into the frame (top-left digits), an
// independent cross-check of the measured latency.
Window {
    id: win
    width: 1280
    height: 800
    visible: false  // shown from C++ after the scene graph signals are connected
    color: "black"
    title: "dc-v6-preview"

    VideoItem {
        id: video
        objectName: "video"
        anchors.fill: parent
    }

    FrameAnimation {
        running: true
        onTriggered: clock.text = video.clockText()
    }

    Rectangle {
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        width: col.width + 24
        height: col.height + 16
        color: "#c0000000"

        Column {
            id: col
            x: 12
            y: 8
            spacing: 4
            Text { id: clock; color: "white"; font.family: "Consolas"; font.pixelSize: 48 }
            Text { text: video.info; color: "#dddddd"; font.family: "Consolas"; font.pixelSize: 16 }
            Text { text: video.stats; color: "#80ff80"; font.family: "Consolas"; font.pixelSize: 20 }
        }
    }
}
