import QtQuick

// One mixer strip: name, input, low / high cut, pan, meter + fader, mute / solo. The same item draws
// the master strip (isMaster: output instead of input, no solo; the name opens the output picker).
Rectangle {
    id: strip

    required property int index
    required property int trackId
    required property string name
    required property int channels
    required property string inputText
    required property bool inputOk
    required property real volumeDb
    required property real fader
    required property real pan
    required property bool mute
    required property bool solo
    required property bool lowcutOn
    required property real lowcutHz
    required property bool highcutOn
    required property real highcutHz
    required property real levelL
    required property real levelR
    required property bool clip
    required property bool cue
    required property bool recordArmed

    property QtObject model          // tracks or master (StripModel)
    property Item scroller: null     // the scrolling list, paused while the pan is dragged
    property bool isMaster: false

    signal menuRequested(int row, string name)
    signal outputRequested()

    objectName: (isMaster ? "masterStrip" : "strip_") + (isMaster ? "" : index)
    width: Theme.stripWidth
    color: isMaster ? Theme.masterStrip : index % 2 ? Theme.stripA : Theme.stripB

    // Name; tapping it opens the track menu.
    Rectangle {
        id: header
        anchors { top: parent.top; left: parent.left; right: parent.right; margins: 4 }
        height: Theme.touch
        radius: Theme.radius
        color: headerTap.pressed ? Theme.controlPressed : "transparent"
        Column {
            anchors.centerIn: parent
            width: parent.width - 8
            Text {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                elide: Text.ElideRight
                color: Theme.text
                font.pixelSize: 15
                font.bold: strip.isMaster
                text: strip.name
            }
            Text {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                elide: Text.ElideRight
                color: strip.inputOk ? Theme.subText : Theme.mute
                font.pixelSize: 11
                text: (strip.inputOk ? "" : "切断 ") + strip.inputText
            }
        }
        TapHandler {
            id: headerTap
            onTapped: strip.isMaster ? strip.outputRequested() : strip.menuRequested(strip.index, strip.name)
        }
    }

    Row {
        id: filters
        anchors { top: header.bottom; topMargin: 4; horizontalCenter: parent.horizontalCenter }
        spacing: 4
        FilterButton {
            objectName: "lowcut_" + strip.index
            width: (strip.width - 12) / 2
            label: "LC"
            on: strip.lowcutOn
            hz: strip.lowcutHz
            minHz: 20; maxHz: 2000
            onToggled: strip.model.toggleLowcut(strip.index)
            onHzMoved: (hz) => strip.model.setLowcutHz(strip.index, hz)
            onHzEntry: Sheets.number.open({ title: strip.name + " ローカット", value: strip.lowcutHz, unit: "Hz",
                                            min: 20, max: 2000, decimals: 0 },
                                          v => strip.model.setLowcutHz(strip.index, v))
        }
        FilterButton {
            objectName: "highcut_" + strip.index
            width: (strip.width - 12) / 2
            label: "HC"
            on: strip.highcutOn
            hz: strip.highcutHz
            minHz: 1000; maxHz: 20000
            onToggled: strip.model.toggleHighcut(strip.index)
            onHzMoved: (hz) => strip.model.setHighcutHz(strip.index, hz)
            onHzEntry: Sheets.number.open({ title: strip.name + " ハイカット", value: strip.highcutHz, unit: "Hz",
                                            min: 1000, max: 20000, decimals: 0 },
                                          v => strip.model.setHighcutHz(strip.index, v))
        }
    }

    PanControl {
        id: panControl
        objectName: "pan_" + strip.index
        anchors { top: filters.bottom; topMargin: 6; left: parent.left; right: parent.right; margins: 6 }
        value: strip.pan
        flick: strip.scroller
        onMoved: (v) => strip.model.setPan(strip.index, v)
    }

    Item {
        id: body
        anchors { top: panControl.bottom; bottom: value.top; left: parent.left; right: parent.right; margins: 8 }

        Meter {
            id: meter
            anchors { left: parent.left; top: parent.top; bottom: parent.bottom }
            width: 16
            stereo: strip.channels === 2
            levelL: strip.levelL
            levelR: strip.levelR
            clip: strip.clip
            onClipCleared: strip.model.clearClip(strip.index)
        }
        Fader {
            objectName: "fader_" + (strip.isMaster ? "master" : strip.index)
            anchors { left: meter.right; leftMargin: 6; right: parent.right; top: parent.top; bottom: parent.bottom }
            position: strip.fader
            zeroDbPosition: strip.model.dbToFader(0)
            onMoved: (p) => strip.model.setFader(strip.index, p)
        }
    }

    // Volume in dB; a long press opens the keypad.
    Rectangle {
        id: value
        objectName: "volumeText_" + (strip.isMaster ? "master" : strip.index)
        anchors { bottom: extras.top; bottomMargin: 2; horizontalCenter: parent.horizontalCenter }
        width: strip.width - 16
        height: 30
        radius: Theme.radius
        color: valueTap.pressed ? Theme.controlPressed : "transparent"
        Text {
            anchors.centerIn: parent
            color: Theme.subText
            font.pixelSize: 13
            text: strip.model.dbText(strip.volumeDb) + " dB"
        }
        TapHandler {
            id: valueTap
            onLongPressed: Sheets.number.open({ title: strip.name + " の音量", value: Math.max(-60, strip.volumeDb),
                                                unit: "dB", min: -144, max: 12, decimals: 1 },
                                              v => strip.model.setVolumeDb(strip.index, v))
        }
    }

    // Cue (headphones) and per-track recording.
    Row {
        id: extras
        anchors { bottom: buttons.top; bottomMargin: 4; horizontalCenter: parent.horizontalCenter }
        spacing: 4
        ToggleButton {
            objectName: "cue_" + strip.index
            visible: !strip.isMaster
            width: (strip.width - 20) / 2
            height: 36
            label: "CUE"
            fontSize: 12
            on: strip.cue
            onColor: Theme.cue
            onTapped: strip.model.toggleCue(strip.index)
        }
        ToggleButton {
            objectName: "rec_" + (strip.isMaster ? "master" : strip.index)
            width: strip.isMaster ? strip.width - 16 : (strip.width - 20) / 2
            height: 36
            label: "● REC"
            fontSize: 12
            on: strip.recordArmed
            onColor: Theme.record
            onTapped: strip.model.toggleRecord(strip.index)
        }
    }

    Row {
        id: buttons
        anchors { bottom: parent.bottom; bottomMargin: 8; horizontalCenter: parent.horizontalCenter }
        spacing: 4
        ToggleButton {
            objectName: "mute_" + (strip.isMaster ? "master" : strip.index)
            width: strip.isMaster ? strip.width - 16 : (strip.width - 20) / 2
            label: "M"
            on: strip.mute
            onColor: Theme.mute
            onTapped: strip.model.toggleMute(strip.index)
        }
        ToggleButton {
            objectName: "solo_" + strip.index
            visible: !strip.isMaster
            width: (strip.width - 20) / 2
            label: "S"
            on: strip.solo
            onColor: Theme.solo
            onLabelColor: "#222"
            onTapped: strip.model.toggleSolo(strip.index)
        }
    }
}
