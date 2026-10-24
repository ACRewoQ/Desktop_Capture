import QtQuick

// Local choice (not a parameter) as a row of buttons: tabs inside a page. model: [{key, label}].
Row {
    id: root
    property var model: []
    property var current
    signal selected(var key)

    spacing: 4
    height: Theme.touch
    Repeater {
        model: root.model
        delegate: Rectangle {
            required property var modelData
            readonly property bool isCurrent: root.current === modelData.key
            objectName: "tab_" + modelData.key
            width: Math.max(96, label.implicitWidth + 28)
            height: root.height
            radius: Theme.radius
            color: isCurrent ? Theme.accentDim : tap.pressed ? Theme.controlPressed : Theme.control
            border.width: isCurrent ? 1 : 0
            border.color: Theme.accent
            Text {
                id: label
                anchors.centerIn: parent
                color: "white"
                font.pixelSize: 15
                text: modelData.label
            }
            TapHandler { id: tap; onTapped: root.selected(modelData.key) }
        }
    }
}
