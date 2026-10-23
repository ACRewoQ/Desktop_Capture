import QtQuick

// Lightweight variant: every control is drawn with Rectangle/Text and pointer handlers.
Window {
    id: window
    width: 1280
    height: 800
    visible: true
    visibility: startFullScreen ? Window.FullScreen : Window.Windowed
    color: "#1d2026"
    title: "dc-v5-probe"

    ListView {
        id: list
        objectName: "stripList"
        anchors { top: parent.top; bottom: stats.top; left: parent.left; right: parent.right }
        orientation: ListView.Horizontal
        boundsBehavior: Flickable.StopAtBounds
        snapMode: ListView.SnapToItem
        model: strips
        delegate: Strip { height: list.height }
    }

    StatsBar {
        id: stats
        anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
    }
}
