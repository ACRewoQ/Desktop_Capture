import QtQuick

// Floor of a Qt Quick window: nothing but the status bar.
Window {
    width: 1280
    height: 800
    visible: true
    visibility: startFullScreen ? Window.FullScreen : Window.Windowed
    color: "#1d2026"
    title: "dc-v5-probe (minimal)"

    StatsBar { anchors { left: parent.left; right: parent.right; bottom: parent.bottom } }
}
