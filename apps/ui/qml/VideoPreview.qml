import QtQuick
import DcUi.Params
import DcUi.Preview

// The live preview (video/preview from capd, protocol.md 10). While this tab is shown the UI
// subscribes; leaving it ends the subscription, so capd only encodes while someone watches.
// Tap the picture to hide or show the details line.
Item {
    id: root
    property bool details: true

    Param { id: previewMix; path: "/video/preview/mix" }

    Rectangle {
        id: frame
        anchors { top: parent.top; left: parent.left; right: parent.right; bottom: bar.top; leftMargin: 12; rightMargin: 12; bottomMargin: 8 }
        radius: Theme.radius
        color: "black"
        clip: true

        PreviewVideo {
            id: video
            objectName: "previewVideo"
            anchors.fill: parent
            stream: previewStream
        }

        Text {
            objectName: "previewStatus"
            anchors.centerIn: parent
            width: parent.width - 48
            visible: !video.showing
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            color: Theme.subText
            font.pixelSize: 16
            text: video.error || previewStream.statusText || "映像を待っています"
        }

        Rectangle {
            anchors { left: parent.left; bottom: parent.bottom; margins: 8 }
            visible: root.details && video.showing
            width: info.implicitWidth + 16
            height: info.implicitHeight + 8
            radius: 4
            color: "#a0000000"
            Text {
                id: info
                anchors.centerIn: parent
                color: Theme.text
                font.pixelSize: 13
                text: previewStream.infoText + (video.decoderName.indexOf("[D3D11") >= 0 ? "・GPU デコード" : video.decoderName ? "・CPU デコード" : "")
                      + (video.latencyMs >= 0 ? "・表示まで " + Math.round(video.latencyMs) + " ms" : "")
            }
        }

        TapHandler { onTapped: root.details = !root.details }
    }

    Row {
        id: bar
        anchors { left: parent.left; right: parent.right; bottom: parent.bottom; leftMargin: 12; rightMargin: 12; bottomMargin: 8 }
        height: Theme.touch
        spacing: 12
        Text {
            anchors.verticalCenter: parent.verticalCenter
            color: Theme.subText
            font.pixelSize: 15
            text: "出すミックス"
        }
        ParamPick {
            objectName: "previewMixPick"
            width: Math.max(120, Math.min(260, bar.width - 120 - monitor.width - 12))
            path: "/video/preview/mix"
            title: "プレビューに出すミックス"
            choices: () => Video.mixChoices(true)
        }
        // The sound that goes with the picture (the same control as in the top bar).
        MonitorButton { id: monitor }
    }
}
