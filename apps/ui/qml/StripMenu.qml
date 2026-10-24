import QtQuick
import DcUi.Params

// Track menu opened from the strip name: input, name, mono / stereo, move left / right, delete
// (slide to confirm).
Sheet {
    id: root
    property int row: -1
    property int trackId: -1
    readonly property string base: "/track/" + trackId

    signal inputRequested(int row, string name)

    function open(r, n) {
        row = r
        trackId = tracks.trackIdAt(r)
        title = n
        shown = true
    }

    onClosed: row = -1

    // Follow the strip if it moves (our own move, or another client's change).
    Connections {
        target: tracks
        enabled: root.shown
        function onRowsMoved() { root.close() }
        function onRowsRemoved() { root.close() }
        function onModelReset() { root.close() }
    }

    MenuButton {
        objectName: "chooseInput"
        width: parent.width
        label: "入力を選ぶ"
        onTapped: {
            const r = root.row
            const n = root.title
            root.close()
            root.inputRequested(r, n)
        }
    }
    MenuButton {
        objectName: "renameTrack"
        width: parent.width
        label: "名前を変える"
        onTapped: {
            const path = root.base + "/name"
            const now = root.title
            root.close()
            Sheets.text.open("トラックの名前", now, t => params.set(path, t))
        }
    }
    ParamSegments {
        objectName: "trackChannels"
        width: parent.width
        path: root.trackId > 0 ? root.base + "/channels" : ""
        labels: ({ 1: "モノ", 2: "ステレオ" })
    }
    Row {
        spacing: 8
        MenuButton {
            width: (root.contentWidth - 8) / 2
            label: "← 左へ"
            active: root.row > 0
            onTapped: { tracks.moveTrack(root.row, -1); root.close() }
        }
        MenuButton {
            width: (root.contentWidth - 8) / 2
            label: "右へ →"
            active: root.row >= 0 && root.row < tracks.count - 1
            onTapped: { tracks.moveTrack(root.row, 1); root.close() }
        }
    }
    SlideConfirm {
        objectName: "deleteTrack"
        width: parent.width
        label: "スライドしてトラックを削除"
        onConfirmed: {
            tracks.removeTrack(root.row)
            root.close()
        }
    }
}
