import QtQuick

// One setting: name and explanation on the left, the control on the right. On a narrow screen
// (remote controller held upright) the control goes below the text.
Item {
    id: root
    property string label
    property string note
    property real controlWidth: 300
    default property alias control: slot.data
    readonly property bool narrow: width < controlWidth + 200

    width: parent ? parent.width : 0
    height: (narrow ? texts.height + 8 + slot.height : Math.max(texts.height, slot.height)) + 16

    Rectangle {
        anchors { left: parent.left; right: parent.right; bottom: parent.bottom }
        height: 1
        color: Theme.stripA
    }
    Column {
        id: texts
        anchors {
            left: parent.left
            right: root.narrow ? parent.right : slot.left; rightMargin: root.narrow ? 0 : 16
        }
        y: root.narrow ? 8 : (root.height - height) / 2
        Text {
            width: parent.width
            wrapMode: Text.Wrap
            color: Theme.text
            font.pixelSize: 15
            text: root.label
        }
        Text {
            width: parent.width
            visible: text.length > 0
            wrapMode: Text.Wrap
            color: Theme.subText
            font.pixelSize: 12
            text: root.note
        }
    }
    Item {
        id: slot
        width: root.narrow ? root.width : root.controlWidth
        height: childrenRect.height
        anchors { right: parent.right }
        y: root.narrow ? texts.height + 16 : (root.height - height) / 2
    }
}
