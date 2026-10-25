import QtQuick

// Picks a track's input or the master's output from the engine's device list. The choices are read
// when it opens (StripModel.inputChoices / outputChoices) and again on changes other than meters
// (a device plugged in or out while it is open).
ChoiceSheet {
    id: root
    property int row: -1
    property bool forMaster: false

    function openInput(r, name) {
        forMaster = false
        row = r
        open(name + " の入力", [], null)
        refresh()
    }
    function openOutput() {
        forMaster = true
        row = 0
        open("マスターの出力先", [], null)
        refresh()
    }
    function refresh() {
        if (row < 0) return
        choices = forMaster ? master.outputChoices() : tracks.inputChoices(row)
    }

    onClosed: row = -1
    onPicked: (c) => {
        if (forMaster) master.setOutput(c.target)
        else tracks.setInput(row, c.kind, c.source, c.channel)
    }

    Connections {
        target: tracks
        enabled: root.shown && !root.forMaster
        function onRowsMoved() { root.close() }
        function onRowsRemoved() { root.close() }
        function onModelReset() { root.close() }
        function onDataChanged(tl, br, roles) { if (roles.length === 0) root.refresh() }
    }
    Connections {
        target: master
        enabled: root.shown && root.forMaster
        function onModelReset() { root.close() }
        function onDataChanged(tl, br, roles) { if (roles.length === 0) root.refresh() }
    }
}
