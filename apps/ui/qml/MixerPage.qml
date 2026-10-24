import QtQuick

// Track strips (scroll sideways) with the master on the right, and the track menus.
Item {
    id: page
    readonly property bool swipeable: false // the strips scroll sideways

    function openDemo(name) {
        if (name === "menu") stripMenu.open(0, tracks.count > 0 ? "Track" : "")
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
            height: list.height
            model: tracks
            scroller: list
            onMenuRequested: (row, name) => stripMenu.open(row, name)
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
            onOutputRequested: sourcePicker.openOutput()
        }
    }

    StripMenu {
        id: stripMenu
        onInputRequested: (row, name) => sourcePicker.openInput(row, name)
    }

    SourcePicker { id: sourcePicker; objectName: "sourcePicker" }
}
