import QtQuick

// Monitor audio (monitorAudio): which LAN send stream this device plays, with its level. A tap opens
// the list: off, or one of the engine's LAN send streams (whatever the mixer routes there, usually the
// master). Plays on this device's default output until turned off, whatever the page.
Rectangle {
    id: root
    objectName: "monitorButton"
    width: 120
    height: Theme.touch
    radius: Theme.radius
    color: tap.pressed ? Theme.controlPressed : monitorAudio.stream > 0 ? Theme.accentDim : Theme.control
    opacity: connection.ready ? 1 : 0.5

    function streamName(n) {
        return params.value("/lan/stream/" + n + "/name") || "LAN 送信 " + n
    }
    function choices() {
        const list = [{ label: "オフ", value: 0, selected: monitorAudio.stream === 0 }]
        for (let n = 1; n <= 8; ++n) {
            const b = "/lan/stream/" + n
            const active = params.value(b + "/active") === true
            const subs = Number(params.value(b + "/subscribers") || 0)
            list.push({ label: streamName(n), value: n, selected: monitorAudio.stream === n,
                        detail: params.value(b + "/codec") + (active ? "・送信中" : subs > 0 ? "" : "・出力なしなら無音") })
        }
        return list
    }

    Column {
        anchors { left: parent.left; right: parent.right; leftMargin: 8; rightMargin: 8; verticalCenter: parent.verticalCenter }
        spacing: 3
        Text {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            elide: Text.ElideRight
            color: "white"
            font.pixelSize: 13
            font.bold: monitorAudio.stream > 0
            text: monitorAudio.stream > 0 ? "♪ " + root.streamName(monitorAudio.stream) : "♪ モニター"
        }
        // Level: -60..0 dBFS; orange when the stream is chosen but nothing plays.
        Rectangle {
            width: parent.width
            height: 4
            radius: 2
            color: "#30ffffff"
            visible: monitorAudio.stream > 0
            Rectangle {
                height: parent.height
                radius: 2
                width: parent.width * Math.max(0, Math.min(1, (monitorAudio.level + 60) / 60))
                color: monitorAudio.level > -3 ? Theme.mute : Theme.ok
            }
            Rectangle {
                anchors.fill: parent
                radius: 2
                visible: !monitorAudio.playing
                color: Theme.warn
                opacity: 0.6
            }
        }
    }

    TapHandler {
        id: tap
        enabled: connection.ready
        onTapped: Sheets.choice.open("モニター音声（この端末で聞く）", root.choices(), c => {
            monitorAudio.stream = c.value
            if (c.value > 0) check.restart()
        })
    }
    // Tell why nothing plays a moment after a stream was chosen (no output routed there, ...).
    Timer {
        id: check
        interval: 2500
        onTriggered: if (monitorAudio.stream > 0 && monitorAudio.statusText) Sheets.toast.show(monitorAudio.statusText)
    }
}
