import QtQuick
import QtQuick.Controls.Basic

// Comparison variant: the same strips made of QtQuick.Controls (Basic style) Slider, Button, Label.
// Slider has no double tap reset, so the self test reports double_tap_reset as failed here.
Window {
    id: window
    width: 1280
    height: 800
    visible: true
    visibility: startFullScreen ? Window.FullScreen : Window.Windowed
    color: "#1d2026"
    title: "dc-v5-probe (controls)"

    ListView {
        id: list
        objectName: "stripList"
        anchors { top: parent.top; bottom: stats.top; left: parent.left; right: parent.right }
        orientation: ListView.Horizontal
        boundsBehavior: Flickable.StopAtBounds
        snapMode: ListView.SnapToItem
        model: strips
        delegate: Pane {
            id: strip
            required property int index
            required property string name
            required property real volumeDb
            required property real position
            required property real level
            required property bool mute
            width: 96
            height: list.height
            padding: 8

            Label { id: label; anchors.top: parent.top; width: parent.width; horizontalAlignment: Text.AlignHCenter; elide: Text.ElideRight; text: strip.name }
            Meter {
                anchors { left: parent.left; top: label.bottom; bottom: valueLabel.top; margins: 4 }
                width: 10
                level: strip.level
            }
            Slider {
                id: slider
                objectName: "fader_" + strip.index
                anchors { left: parent.left; leftMargin: 16; right: parent.right; top: label.bottom; bottom: valueLabel.top; margins: 4 }
                orientation: Qt.Vertical
                from: 0
                to: 1
                live: true
                value: strip.position
                onMoved: strips.setPosition(strip.index, slider.value)
                onPressedChanged: probe.faderGrab(strip.index, pressed)
                Component.onCompleted: handle.objectName = "faderThumb_" + strip.index
            }
            Label { id: valueLabel; anchors { bottom: muteButton.top; horizontalCenter: parent.horizontalCenter }
                    text: strip.volumeDb <= -144 ? "-∞" : strip.volumeDb.toFixed(1) + " dB" }
            Button { id: muteButton; anchors.bottom: parent.bottom; width: parent.width; text: "M"; checkable: true
                     checked: strip.mute; onClicked: strips.toggleMute(strip.index) }
        }
    }

    StatsBar {
        id: stats
        anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
    }
}
