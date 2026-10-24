import QtQuick

// Picks a track's input or the master's output from the engine's device list. The choices are read
// when it opens (StripModel.inputChoices / outputChoices) and again on changes other than meters
// (a device plugged in or out while it is open).
Item {
    id: root
    property int row: -1
    property bool forMaster: false
    property string title
    property var choices: []
    visible: row >= 0

    function openInput(r, name) {
        forMaster = false
        title = name + " の入力"
        row = r
        refresh()
    }
    function openOutput() {
        forMaster = true
        title = "マスターの出力先"
        row = 0
        refresh()
    }
    function refresh() {
        if (row < 0) return
        choices = forMaster ? master.outputChoices() : tracks.inputChoices(row)
    }
    function close() { row = -1 }
    function pick(c) {
        if (forMaster) master.setOutput(c.target)
        else tracks.setInput(row, c.kind, c.source, c.channel)
        close()
    }

    Connections {
        target: tracks
        enabled: root.visible && !root.forMaster
        function onRowsMoved() { root.close() }
        function onRowsRemoved() { root.close() }
        function onModelReset() { root.close() }
        function onDataChanged(tl, br, roles) { if (roles.length === 0) root.refresh() }
    }
    Connections {
        target: master
        enabled: root.visible && root.forMaster
        function onModelReset() { root.close() }
        function onDataChanged(tl, br, roles) { if (roles.length === 0) root.refresh() }
    }

    Rectangle {
        anchors.fill: parent
        color: "#80000000"
        TapHandler { onTapped: root.close() }
    }

    Rectangle {
        anchors.centerIn: parent
        width: Math.min(420, root.width - 32)
        height: Math.min(header.height + list.contentHeight + closeButton.height + 48, root.height - 32)
        radius: 10
        color: Theme.panel
        border.color: "#444a53"
        TapHandler {} // keep taps on the panel from closing it

        Text {
            id: header
            anchors { top: parent.top; left: parent.left; right: parent.right; margins: 16 }
            horizontalAlignment: Text.AlignHCenter
            elide: Text.ElideRight
            color: Theme.text
            font.pixelSize: 18
            text: root.title
        }

        Flickable {
            id: list
            anchors { top: header.bottom; bottom: closeButton.top; left: parent.left; right: parent.right; margins: 16 }
            clip: true
            contentHeight: column.implicitHeight
            boundsBehavior: Flickable.StopAtBounds

            Column {
                id: column
                width: list.width
                spacing: 6
                Repeater {
                    model: root.choices
                    delegate: Rectangle {
                        required property var modelData
                        required property int index
                        objectName: "choice_" + index
                        width: column.width
                        height: 52
                        radius: Theme.radius
                        color: tap.pressed ? Theme.controlPressed : Theme.control
                        border.width: modelData.selected ? 2 : 0
                        border.color: Theme.accent
                        Column {
                            anchors { verticalCenter: parent.verticalCenter; left: parent.left; right: parent.right; margins: 12 }
                            Text {
                                width: parent.width
                                elide: Text.ElideRight
                                color: "white"
                                font.pixelSize: 15
                                text: modelData.label
                            }
                            Text {
                                width: parent.width
                                visible: text.length > 0
                                elide: Text.ElideRight
                                color: Theme.subText
                                font.pixelSize: 11
                                text: modelData.detail
                            }
                        }
                        TapHandler { id: tap; onTapped: root.pick(modelData) }
                    }
                }
            }
        }

        MenuButton {
            id: closeButton
            anchors { bottom: parent.bottom; left: parent.left; right: parent.right; margins: 16 }
            label: "閉じる"
            onTapped: root.close()
        }
    }
}
