pragma Singleton
import QtQuick

// Colours and sizes shared by every screen. Touch targets are at least 44 px (V5).
QtObject {
    readonly property color background: "#1d2026"
    readonly property color panel: "#15171b"
    readonly property color stripA: "#23262d"
    readonly property color stripB: "#262a31"
    readonly property color masterStrip: "#2b2733"
    readonly property color control: "#3a3f47"
    readonly property color controlPressed: "#4a515b"
    readonly property color text: "#e0e3e8"
    readonly property color subText: "#9aa1aa"
    readonly property color accent: "#4fc3f7"
    readonly property color accentDim: "#1e6f94"
    readonly property color border: "#444a53"
    readonly property color nav: "#121418"
    readonly property color mute: "#e53935"
    readonly property color solo: "#fdd835"
    readonly property color filter: "#26a69a"
    readonly property color record: "#e53935"
    readonly property color ok: "#66bb6a"
    readonly property color warn: "#ffca28"
    readonly property int stripWidth: 108
    readonly property int touch: 44
    readonly property int navWidth: 84
    readonly property int radius: 6
}
