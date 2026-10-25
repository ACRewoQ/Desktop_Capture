import QtQuick

// Picks one entry from a list: open(title, choices, callback) with choices as
// [{label, detail?, selected?, disabled?, ...}] or {header} lines between groups; callback(choice) runs
// with the tapped entry.
Sheet {
    id: root
    property var choices: []
    property var callback: null

    signal picked(var choice)

    function open(t, list, cb) {
        title = t
        choices = list
        callback = cb || null
        shown = true
    }

    onClosed: callback = null

    Repeater {
        model: root.choices
        delegate: Rectangle {
            required property var modelData
            required property int index
            readonly property bool isHeader: modelData.header !== undefined
            objectName: isHeader ? "" : "choice_" + index
            width: parent ? parent.width : 0
            height: isHeader ? 30 : modelData.detail ? 56 : 48
            radius: Theme.radius
            color: isHeader ? "transparent" : tap.pressed ? Theme.controlPressed : Theme.control
            border.width: modelData.selected ? 2 : 0
            border.color: Theme.accent
            opacity: modelData.disabled ? 0.45 : 1
            Text {
                visible: parent.isHeader
                anchors { left: parent.left; bottom: parent.bottom; bottomMargin: 4 }
                color: Theme.accent
                font.pixelSize: 13
                font.bold: true
                text: modelData.header || ""
            }
            Column {
                visible: !parent.isHeader
                anchors { verticalCenter: parent.verticalCenter; left: parent.left; right: parent.right; margins: 12 }
                Text {
                    width: parent.width
                    elide: Text.ElideRight
                    color: "white"
                    font.pixelSize: 15
                    text: modelData.label || ""
                }
                Text {
                    width: parent.width
                    visible: !!modelData.detail
                    elide: Text.ElideRight
                    color: Theme.subText
                    font.pixelSize: 11
                    text: modelData.detail || ""
                }
            }
            TapHandler {
                id: tap
                enabled: !parent.isHeader && !modelData.disabled
                onTapped: {
                    const cb = root.callback
                    const c = modelData
                    root.picked(c)
                    root.close()
                    if (cb) cb(c) // after close, so that it can open the next sheet
                }
            }
        }
    }
}
