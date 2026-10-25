import QtQuick
import DcUi.Params

// The master strip's settings, opened from its name: outputs, recording, the cue (headphone) bus.
Sheet {
    id: root
    function open() { shown = true }

    Param { id: name; path: "/master/name" }
    title: name.value || "Master"

    panelWidth: 560

    SettingSection { text: "出力先"; topPadding: 0 }
    TargetList {
        objectName: "masterOutputs"
        base: "/master/out"
        use: "master"
    }

    SettingSection { text: "録音" }
    SettingRow {
        label: "マスターを録音する"
        note: "WAV / FLAC（録音の設定は録画ページ）"
        controlWidth: 100
        ParamSwitch { path: "/master/record/enabled" }
    }
    SettingRow {
        label: "録音する位置"
        controlWidth: 240
        ParamSegments {
            path: "/master/record/tap"
            labels: ({ pre: "フェーダー前", post: "フェーダー後" })
        }
    }

    SettingSection { text: "キュー（ヘッドホン）" }
    SettingRow {
        label: "キューの出力先"
        note: "CUE を押したトラックがここに出ます"
        controlWidth: 240
        ParamPick {
            objectName: "cueTarget"
            path: "/cue/target"
            title: "キューの出力先"
            choices: function() { return routes.targetChoices("cue") }
        }
    }
    SettingRow {
        label: "キューの音量"
        controlWidth: 160
        ParamNumber {
            path: "/cue/volume_db"
            title: "キューの音量"
            unit: "dB"
            format: v => (v <= -60 ? "-∞" : (v > 0 ? "+" : "") + Number(v).toFixed(1)) + " dB"
        }
    }

    SettingSection { text: "名前" }
    MenuButton {
        width: parent.width
        label: "名前を変える"
        onTapped: {
            const now = root.title
            root.close()
            Sheets.text.open("マスターの名前", now, t => params.set("/master/name", t))
        }
    }
}
