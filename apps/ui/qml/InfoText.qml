import QtQuick

// Read-only value at the right side of a SettingRow.
Text {
    width: parent ? parent.width : 0
    height: Theme.touch
    verticalAlignment: Text.AlignVCenter
    horizontalAlignment: Text.AlignRight
    elide: Text.ElideMiddle
    color: Theme.text
    font.pixelSize: 15
}
