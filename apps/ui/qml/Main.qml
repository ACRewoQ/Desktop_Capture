import QtQuick

// Mixer screen: top bar (connection, recording), track strips (scrolls sideways), master on the
// right. Plain items and pointer handlers only; no QtQuick.Controls (V5).
Window {
    id: window
    width: 1280
    height: 800
    minimumWidth: 640
    minimumHeight: 480
    visible: true
    visibility: startFullScreen ? Window.FullScreen : Window.Windowed
    color: Theme.background
    title: "Desktop Capture ミキサー"

    TopBar {
        id: topBar
        anchors { top: parent.top; left: parent.left; right: parent.right }
    }

    ListView {
        id: list
        objectName: "trackList"
        anchors { top: topBar.bottom; bottom: parent.bottom; left: parent.left; right: masterStrip.left; rightMargin: 4 }
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
        anchors { top: topBar.bottom; bottom: parent.bottom; right: parent.right }
        width: Theme.stripWidth
        interactive: false
        model: master
        delegate: Strip {
            height: masterStrip.height
            model: master
            isMaster: true
        }
    }

    // Shown while the engine is not reachable; the last known values stay visible but dimmed.
    Rectangle {
        anchors { top: topBar.bottom; bottom: parent.bottom; left: parent.left; right: parent.right }
        visible: !connection.ready
        color: "#a0101216"
        TapHandler {} // swallow touches while disconnected
        Text {
            anchors.centerIn: parent
            color: Theme.text
            font.pixelSize: 20
            text: connection.statusText + "\n" + connection.address
            horizontalAlignment: Text.AlignHCenter
        }
    }

    StripMenu { id: stripMenu; anchors.fill: parent }

    Toast { id: toast }

    Connections {
        target: tracks
        function onFailed(message) { toast.show(message) }
    }
    Connections {
        target: master
        function onFailed(message) { toast.show(message) }
    }
    Connections {
        target: recorder
        function onFailed(message) { toast.show("録画: " + message) }
    }
}
