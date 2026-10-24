import QtQuick

// One line of text (names). open(title, text, callback); callback(text) runs on 決定 or Enter.
Sheet {
    id: root
    property var callback: null
    property int maxLength: 64
    showClose: false

    function open(t, value, cb) {
        title = t
        input.text = value || ""
        callback = cb || null
        shown = true
        input.forceActiveFocus()
        input.selectAll()
    }
    function accept() {
        const cb = callback
        const v = input.text.trim()
        close()
        if (cb) cb(v)
    }

    onClosed: { callback = null; input.focus = false }

    Rectangle {
        width: parent ? parent.width : 0
        height: 52
        radius: Theme.radius
        color: Theme.background
        border.color: input.activeFocus ? Theme.accent : Theme.border
        TextInput {
            id: input
            objectName: "textEntry"
            anchors { fill: parent; leftMargin: 12; rightMargin: 12 }
            verticalAlignment: TextInput.AlignVCenter
            color: Theme.text
            selectionColor: Theme.accentDim
            font.pixelSize: 18
            maximumLength: root.maxLength
            clip: true
            onAccepted: root.accept()
        }
        TapHandler { onTapped: input.forceActiveFocus() }
    }
    Row {
        spacing: 8
        MenuButton {
            width: (root.contentWidth - 8) / 2
            label: "やめる"
            onTapped: root.close()
        }
        MenuButton {
            objectName: "textOk"
            width: (root.contentWidth - 8) / 2
            label: "決定"
            color: Theme.accentDim
            onTapped: root.accept()
        }
    }
}
