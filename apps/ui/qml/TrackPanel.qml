import QtQuick
import DcUi.Params

// Everything about one track, opened from the strip name: input, outputs, sends, cue and per-track
// recording, then name, mono / stereo, order and delete (slide to confirm).
Sheet {
    id: root
    property int row: -1
    property int trackId: -1
    readonly property string base: "/track/" + trackId
    property string inputText

    signal inputRequested(int row, string name)

    function open(r, n) {
        row = r
        trackId = tracks.trackIdAt(r)
        title = n
        inputText = tracks.inputTextAt(r)
        shown = true
    }

    panelWidth: 560
    onClosed: row = -1

    // Follow the strip if it moves (our own move, or another client's change).
    Connections {
        target: tracks
        enabled: root.shown
        function onRowsMoved() { root.close() }
        function onRowsRemoved() { root.close() }
        function onModelReset() { root.close() }
        function onDataChanged(tl, br, roles) { if (roles.length === 0 && root.row >= 0) root.inputText = tracks.inputTextAt(root.row) }
    }

    Param { id: inputKind; path: root.trackId > 0 ? root.base + "/input/kind" : "" }
    Param { id: inputState; path: root.trackId > 0 ? root.base + "/input/state" : "" }
    Param { id: lanDelay; path: root.trackId > 0 ? root.base + "/input/lan_delay_ms" : "" }

    SettingSection { text: "入力"; topPadding: 0 }
    MenuButton {
        objectName: "chooseInput"
        width: parent.width
        label: (inputState.value === "disconnected" ? "切断中: " : "") + root.inputText
        onTapped: {
            const r = root.row
            const n = root.title
            root.close()
            root.inputRequested(r, n)
        }
    }
    SettingRow {
        visible: inputKind.value === "lan"
        label: "LAN の受信バッファ"
        note: "今 " + Number(lanDelay.value).toFixed(0) + " ms。0 は自動"
        controlWidth: 160
        ParamNumber { path: root.trackId > 0 ? root.base + "/input/lan_buffer_ms" : ""; title: "受信バッファ"; unit: "ms"; decimals: 0 }
    }

    SettingSection { text: "出力先" }
    TargetList {
        objectName: "trackOutputs"
        base: root.trackId > 0 ? root.base + "/out" : ""
        use: "out"
        trackId: root.trackId
    }

    SettingSection { text: "センド" }
    TargetList {
        objectName: "trackSends"
        base: root.trackId > 0 ? root.base + "/send" : ""
        use: "send"
        trackId: root.trackId
    }

    SettingSection { text: "モニターと録音" }
    SettingRow {
        label: "キュー（ヘッドホンで確認）"
        controlWidth: 100
        ParamSwitch { path: root.trackId > 0 ? root.base + "/cue" : "" }
    }
    SettingRow {
        label: "トラック別に録音する"
        note: "WAV / FLAC（録音の設定は録画ページ）"
        controlWidth: 100
        ParamSwitch { path: root.trackId > 0 ? root.base + "/record/enabled" : "" }
    }
    SettingRow {
        label: "録音する位置"
        controlWidth: 240
        ParamSegments {
            path: root.trackId > 0 ? root.base + "/record/tap" : ""
            labels: ({ pre: "フェーダー前", post: "フェーダー後" })
        }
    }

    SettingSection { text: "トラック" }
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
