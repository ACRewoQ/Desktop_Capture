import QtQuick

// Track strips (scroll sideways) with the master on the right, and the track menus. Two fingers
// pinched sideways change the width of the track strips (narrow: more tracks on screen); the strip
// under the fingers stays where it is.
Item {
    id: page
    objectName: "mixerPage"
    readonly property bool swipeable: false // the strips scroll sideways
    readonly property real minStripWidth: 72
    readonly property real maxStripWidth: 216
    property real stripWidth: Theme.stripWidth
    // While pinching: the width at the start times how far the fingers spread since then.
    property real pinchBaseWidth: Theme.stripWidth
    property real pinchBaseScale: 1
    property real pinchX: 0           // where the fingers are, in the list
    property real pinchTrack: 0       // and which track (fractional) is there
    readonly property real pinchedWidth: pinch.active
        ? Math.max(minStripWidth, Math.min(maxStripWidth, pinchBaseWidth * pinch.scale / Math.max(0.01, pinchBaseScale)))
        : stripWidth

    onPinchedWidthChanged: {
        if (!pinch.active) return
        stripWidth = Math.round(pinchedWidth)
        list.contentX = Math.max(0, Math.min(list.contentWidth - list.width, pinchTrack * stripWidth - pinchX))
    }

    function openDemo(name) {
        if (name === "menu") trackPanel.open(0, "Track")
        else if (name === "master") masterPanel.open()
        else if (name === "fx") fxPanel.open("/track/1", "Track")
        else if (name === "fxedit") fxEditor.open("/track/1/fx/1")
        else if (name === "input") sourcePicker.openInput(0, "Track")
        else if (name === "output") sourcePicker.openOutput()
    }

    ListView {
        id: list
        objectName: "trackList"
        anchors { top: parent.top; bottom: parent.bottom; left: parent.left; right: masterStrip.left; rightMargin: 4 }
        orientation: ListView.Horizontal
        boundsBehavior: Flickable.StopAtBounds
        clip: true
        model: tracks
        delegate: Strip {
            width: page.stripWidth
            height: list.height
            model: tracks
            scroller: list
            onMenuRequested: (row, name) => trackPanel.open(row, name)
            onFxRequested: (base, name) => fxPanel.open(base, name)
        }
        footer: AddTrackTile { height: list.height }

        add: Transition { NumberAnimation { property: "opacity"; from: 0; to: 1; duration: 150 } }
        displaced: Transition { NumberAnimation { property: "x"; duration: 150; easing.type: Easing.OutQuad } }
        move: Transition { NumberAnimation { property: "x"; duration: 150; easing.type: Easing.OutQuad } }

        Connections {
            target: tracks
            function onTrackAdded(row) { list.positionViewAtIndex(row, ListView.Contain) }
        }
    }

    PinchHandler {
        id: pinch
        objectName: "stripPinch"
        target: null
        minimumPointCount: 2
        maximumPointCount: 2
        minimumRotation: 0
        maximumRotation: 0
        onActiveChanged: {
            if (!active) return
            page.pinchBaseWidth = page.stripWidth
            page.pinchBaseScale = scale
            page.pinchX = Math.max(0, Math.min(list.width, centroid.position.x - list.x))
            page.pinchTrack = (list.contentX + page.pinchX) / page.stripWidth
        }
    }

    // The master strip stays visible while the tracks scroll.
    ListView {
        id: masterStrip
        anchors { top: parent.top; bottom: parent.bottom; right: parent.right }
        width: Theme.stripWidth
        interactive: false
        model: master
        delegate: Strip {
            height: masterStrip.height
            model: master
            isMaster: true
            onOutputRequested: masterPanel.open()
            onFxRequested: (base, name) => fxPanel.open(base, name)
        }
    }

    TrackPanel {
        id: trackPanel
        objectName: "trackPanel"
        onInputRequested: (row, name) => sourcePicker.openInput(row, name)
    }
    MasterPanel { id: masterPanel; objectName: "masterPanel" }
    FxPanel {
        id: fxPanel
        objectName: "fxPanel"
        onEditRequested: (slot) => fxEditor.open(slot)
    }
    FxEditor { id: fxEditor; objectName: "fxEditor" }

    SourcePicker { id: sourcePicker; objectName: "sourcePicker" }
}
