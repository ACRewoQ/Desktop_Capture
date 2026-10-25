import QtQuick

// Read-only setting: name on the left, value on the right (stays on one line on narrow screens).
SettingRow {
    id: root
    property alias value: info.text
    property alias valueColor: info.color
    controlWidth: Math.max(140, Math.min(info.implicitWidth + 8, width * 0.6))
    InfoText { id: info }
}
