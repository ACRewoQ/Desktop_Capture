import QtQuick

// Track menu opened from the strip name: input, move left / right, delete (asks twice).
Item {
    id: root
    property int row: -1
    property string name
    property bool confirmDelete: false
    visible: row >= 0

    signal inputRequested(int row, string name)

    function open(r, n) { row = r; name = n; confirmDelete = false }
    function close() { row = -1 }

    // Follow the strip if it moves (our own move, or another client's change).
    Connections {
        target: tracks
        function onRowsMoved() { root.close() }
        function onRowsRemoved() { root.close() }
        function onModelReset() { root.close() }
    }

    Rectangle {
        anchors.fill: parent
        color: "#80000000"
        TapHandler { onTapped: root.close() }
    }

    Rectangle {
        anchors.centerIn: parent
        width: 320
        height: column.implicitHeight + 32
        radius: 10
        color: Theme.panel
        border.color: "#444a53"
        TapHandler {} // keep taps on the panel from closing it

        Column {
            id: column
            anchors { fill: parent; margins: 16 }
            spacing: 10

            Text {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                elide: Text.ElideRight
                color: Theme.text
                font.pixelSize: 18
                text: root.name
            }
            MenuButton {
                objectName: "chooseInput"
                width: parent.width
                label: "入力を選ぶ"
                onTapped: {
                    const r = root.row
                    const n = root.name
                    root.close()
                    root.inputRequested(r, n)
                }
            }
            Row {
                spacing: 10
                MenuButton {
                    width: (column.width - 10) / 2
                    label: "← 左へ"
                    active: root.row > 0
                    onTapped: { tracks.moveTrack(root.row, -1); root.close() }
                }
                MenuButton {
                    width: (column.width - 10) / 2
                    label: "右へ →"
                    active: root.row >= 0 && root.row < tracks.count - 1
                    onTapped: { tracks.moveTrack(root.row, 1); root.close() }
                }
            }
            MenuButton {
                objectName: "deleteTrack"
                width: parent.width
                label: root.confirmDelete ? "もう一度押すと削除します" : "トラックを削除"
                color: root.confirmDelete ? Theme.mute : Theme.control
                onTapped: {
                    if (!root.confirmDelete) {
                        root.confirmDelete = true
                        return
                    }
                    tracks.removeTrack(root.row)
                    root.close()
                }
            }
            MenuButton {
                width: parent.width
                label: "閉じる"
                onTapped: root.close()
            }
        }
    }
}
