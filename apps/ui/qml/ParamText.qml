import QtQuick
import DcUi.Params

// String parameter (names, file paths, text): shows the value, a tap opens the text entry.
Rectangle {
    id: root
    property alias path: param.path
    property string title
    property string emptyText: "なし"
    property bool multiline: false
    readonly property alias param: param

    width: parent ? parent.width : 0
    height: multiline ? Math.max(Theme.touch, Math.min(shown.implicitHeight + 16, 120)) : Theme.touch
    radius: Theme.radius
    color: tap.pressed ? Theme.controlPressed : Theme.control
    opacity: param.readOnly ? 0.5 : 1
    clip: true
    Param { id: param }

    Text {
        id: shown
        anchors { left: parent.left; right: parent.right; margins: 12; verticalCenter: parent.verticalCenter }
        elide: Text.ElideMiddle
        wrapMode: root.multiline ? Text.Wrap : Text.NoWrap
        maximumLineCount: root.multiline ? 4 : 1
        color: param.value ? "white" : Theme.subText
        font.pixelSize: 15
        text: param.value ? String(param.value) : root.emptyText
    }
    TapHandler {
        id: tap
        enabled: !param.readOnly
        onTapped: Sheets.text.open(root.title, param.value || "", t => param.set(t), root.multiline)
    }
}
