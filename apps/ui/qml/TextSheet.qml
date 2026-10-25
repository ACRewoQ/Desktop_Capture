import QtQuick

// Text entry. open(title, text, callback[, multiline]); callback(text) runs on 決定 (or Enter for one
// line). One line (names) by default; multiline is for the text source, where Enter starts a new line.
Sheet {
    id: root
    property var callback: null
    property int maxLength: 1024
    property bool multiline: false
    showClose: false
    panelWidth: multiline ? 560 : 420

    function open(t, value, cb, lines) {
        title = t
        multiline = lines === true
        callback = cb || null
        shown = true
        const field = multiline ? area : input
        field.text = value || ""
        field.forceActiveFocus()
        field.selectAll()
    }
    function accept() {
        const cb = callback
        const v = multiline ? area.text : input.text.trim()
        close()
        if (cb) cb(v)
    }

    onClosed: { callback = null; input.focus = false; area.focus = false }

    Rectangle {
        visible: !root.multiline
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
    Rectangle {
        visible: root.multiline
        width: parent ? parent.width : 0
        height: 180
        radius: Theme.radius
        color: Theme.background
        border.color: area.activeFocus ? Theme.accent : Theme.border
        Flickable {
            id: areaFlick
            anchors { fill: parent; margins: 10 }
            contentHeight: area.contentHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            TextEdit {
                id: area
                objectName: "textArea"
                width: areaFlick.width
                height: Math.max(areaFlick.height, contentHeight)
                wrapMode: TextEdit.Wrap
                color: Theme.text
                selectionColor: Theme.accentDim
                font.pixelSize: 18
                onCursorRectangleChanged: {
                    const r = cursorRectangle
                    if (r.y < areaFlick.contentY) areaFlick.contentY = r.y
                    else if (r.y + r.height > areaFlick.contentY + areaFlick.height)
                        areaFlick.contentY = r.y + r.height - areaFlick.height
                }
            }
        }
        TapHandler { onTapped: area.forceActiveFocus() }
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
