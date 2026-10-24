import QtQuick
import DcUi.Params

// Settings of one sampler pad: the sound file, name, colour, what a press does, volume, test and clear.
Sheet {
    id: root
    property int bank: 0
    property int pad: 0
    readonly property string base: pad > 0 ? "/sampler/bank/" + bank + "/pad/" + pad : ""

    function open(b, p) {
        bank = b
        pad = p
        title = "バンク " + b + " のパッド " + p
        shown = true
    }
    function durationText(ms) {
        const s = Number(ms || 0) / 1000
        return s >= 60 ? Math.floor(s / 60) + " 分 " + Math.round(s % 60) + " 秒" : s.toFixed(1) + " 秒"
    }
    // "#rrggbb" <-> 0xRRGGBB for the shared colour sheet.
    function pickColor() {
        const now = color_.value ? parseInt(String(color_.value).substring(1), 16) : 0x3f5a73
        Sheets.color.open({ title: "パッドの色", value: now, alpha: false },
                          v => color_.set("#" + ("00000" + Math.round(v).toString(16)).slice(-6)))
    }

    panelWidth: 560
    onClosed: pad = 0

    Param { id: state_; path: root.base ? root.base + "/state" : "" }
    Param { id: error_; path: root.base ? root.base + "/error" : "" }
    Param { id: duration_; path: root.base ? root.base + "/duration_ms" : "" }
    Param { id: color_; path: root.base ? root.base + "/color" : "" }

    SettingSection { text: "音"; topPadding: 0 }
    SettingRow {
        label: "ファイル"
        note: "エンジンの PC のパス。WAV・FLAC・MP3・Ogg、10 分まで"
        ParamText { objectName: "padFile"; path: root.base ? root.base + "/file" : ""; title: "音声ファイルのパス"; emptyText: "空" }
    }
    InfoRow {
        label: "状態"
        value: state_.value === "ready" ? "使えます（" + root.durationText(duration_.value) + "）"
             : state_.value === "loading" ? "読み込み中…" : state_.value === "error" ? "読めません" : "空"
        valueColor: state_.value === "ready" ? Theme.ok : state_.value === "error" ? Theme.mute : Theme.subText
        note: state_.value === "error" ? error_.value : ""
    }
    SettingRow {
        label: "押したとき"
        note: "1 回 = 最後まで（押し直すと頭から）、押すたび = 再生・停止、押す間 = 押している間だけ、ループ = 押すたびにループ・停止"
        controlWidth: 360
        ParamSegments {
            path: root.base ? root.base + "/mode" : ""
            labels: ({ oneshot: "1 回", toggle: "押すたび", hold: "押す間", loop: "ループ" })
        }
    }
    SettingRow {
        label: "音量"
        controlWidth: 160
        ParamNumber { path: root.base ? root.base + "/volume_db" : ""; title: "パッドの音量"; unit: "dB" }
    }

    SettingSection { text: "見た目" }
    SettingRow {
        label: "名前"
        note: "空ならファイル名"
        ParamText { path: root.base ? root.base + "/name" : ""; title: "パッドの名前"; emptyText: "（ファイル名）" }
    }
    SettingRow {
        label: "色"
        controlWidth: 220
        Row {
            spacing: 6
            Rectangle {
                width: 150; height: Theme.touch; radius: Theme.radius
                color: Theme.control
                Rectangle { anchors { fill: parent; margins: 6 } radius: 4; color: color_.value ? color_.value : "#3f5a73" }
                TapHandler { onTapped: root.pickColor() }
            }
            MenuButton { width: 64; height: Theme.touch; fontSize: 13; label: "既定"; onTapped: color_.set("") }
        }
    }

    Item { width: 1; height: 8 }
    Row {
        spacing: 8
        MenuButton {
            width: (root.contentWidth - 8) / 2
            label: "▶ 鳴らしてみる"
            active: state_.value === "ready"
            onTapped: params.call("/sampler/trigger", { pad: root.pad, bank: root.bank })
        }
        MenuButton {
            objectName: "clearPad"
            width: (root.contentWidth - 8) / 2
            label: "空にする"
            onTapped: {
                const b = root.base
                const ch = {}
                ch[b + "/file"] = ""
                ch[b + "/name"] = ""
                ch[b + "/color"] = ""
                params.setMany(ch)
            }
        }
    }
}
