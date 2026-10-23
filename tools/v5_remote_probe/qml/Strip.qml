import QtQuick

// One mixer strip: name, meter, fader, value and mute. Plain items only (no QtQuick.Controls).
Rectangle {
    id: strip

    required property int index
    required property string name
    required property real volumeDb
    required property real position
    required property real level
    required property bool mute

    width: 96
    color: index % 2 ? "#23262d" : "#262a31"

    Text {
        id: label
        objectName: "stripLabel_" + strip.index
        anchors { top: parent.top; topMargin: 8; horizontalCenter: parent.horizontalCenter }
        width: parent.width - 8
        horizontalAlignment: Text.AlignHCenter
        elide: Text.ElideRight
        color: "#e0e3e8"
        font.pixelSize: 15
        text: strip.name
    }

    Item {
        id: body
        anchors { top: label.bottom; bottom: value.top; left: parent.left; right: parent.right; margins: 8 }

        Meter {
            anchors { left: parent.left; top: parent.top; bottom: parent.bottom }
            width: 10
            level: strip.level
        }
        Fader {
            anchors { left: parent.left; leftMargin: 14; right: parent.right; top: parent.top; bottom: parent.bottom }
            row: strip.index
            position: strip.position
            zeroDbPosition: strips.dbToPosition(0)
            onMoved: (p) => strips.setPosition(strip.index, p)
            onGrabChanged: (g) => probe.faderGrab(strip.index, g)
        }
    }

    Text {
        id: value
        anchors { bottom: muteButton.top; bottomMargin: 6; horizontalCenter: parent.horizontalCenter }
        color: "#aab0b8"
        font.pixelSize: 13
        text: strip.volumeDb <= -144 ? "-∞" : strip.volumeDb.toFixed(1) + " dB"
    }

    Rectangle {
        id: muteButton
        anchors { bottom: parent.bottom; bottomMargin: 8; horizontalCenter: parent.horizontalCenter }
        width: parent.width - 16
        height: 44
        radius: 6
        color: strip.mute ? "#e53935" : "#3a3f47"
        Text { anchors.centerIn: parent; text: "M"; color: "white"; font.pixelSize: 16; font.bold: true }
        TapHandler { onTapped: strips.toggleMute(strip.index) }
    }
}
