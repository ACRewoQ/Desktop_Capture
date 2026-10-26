import QtQuick
import DcUi.Params

// One hotkey / MIDI binding: what starts it (learned on the engine PC or typed), which MIDI device,
// what it does, on / off, its state, delete.
Sheet {
    id: root
    property int num: 0
    readonly property string base: num > 0 ? "/control/binding/" + num : ""
    readonly property string mode: Bindings.modeOf(action_.value, target_.value)
    readonly property bool learning: num > 0 && learnBinding.value === num
    readonly property bool isMidi: String(trigger_.value || "").startsWith("midi:")

    function open(n) {
        num = n
        title = "割り当て " + n
        shown = true
    }
    function learn(source) { params.call("/control/learn", { binding: num, source: source }) }
    function setMode(m) { params.setMany(prefixed(Bindings.modeDefaults(m))) }
    function prefixed(fields) {
        const ch = {}
        for (const k in fields) ch[base + "/" + k] = fields[k]
        return ch
    }
    function pick(title, list, cb) { Sheets.choice.open(title, list, c => cb(c.value)) }
    function pickTarget() {
        if (mode === "set") {
            Sheets.text.open("パラメーターのパス（例 /track/1/mute）", target_.value, t => target_.set(t.trim()))
            return
        }
        pick("どのトラック", Bindings.stripChoices(target_.value), strip =>
             pick(Bindings.stripName(strip), Bindings.paramChoices(strip, mode), p => target_.set(p)))
    }
    // pad target: "<pad>" (active bank) or "<bank>/<pad>"
    readonly property var padParts: String(target_.value || "").split("/")
    readonly property int padBank: padParts.length === 2 ? Number(padParts[0]) : 0
    readonly property int padNum: Number(padParts[padParts.length - 1]) || 1
    function setPad(bank, pad) { target_.set(bank > 0 ? bank + "/" + pad : String(pad)) }
    readonly property int sceneMix: mode === "scene" ? Number(String(target_.value).split("/")[3]) : 0

    panelWidth: 620
    onClosed: {
        if (learning) params.call("/control/learn/cancel")
        num = 0
    }

    Param { id: trigger_; path: root.base ? root.base + "/trigger" : "" }
    Param { id: device_; path: root.base ? root.base + "/device" : "" }
    Param { id: action_; path: root.base ? root.base + "/action" : "" }
    Param { id: target_; path: root.base ? root.base + "/target" : "" }
    Param { id: value_; path: root.base ? root.base + "/value" : "" }
    Param { id: state_; path: root.base ? root.base + "/state" : "" }
    Param { id: error_; path: root.base ? root.base + "/error" : "" }
    Param { id: learnBinding; path: "/control/learn/binding" }
    Param { id: learnSource; path: "/control/learn/source" }

    SettingSection { text: "きっかけ"; topPadding: 0 }
    SettingRow {
        label: Bindings.triggerText(trigger_.value)
        note: root.learning ? "" : "「学習」を押してから、エンジンの PC でキーを押すか MIDI 機器を動かします"
        controlWidth: 230
        Row {
            spacing: 6
            MenuButton {
                objectName: "learnButton"
                width: 112; height: Theme.touch; fontSize: 14
                label: root.learning ? "やめる" : "学習"
                onTapped: root.learning ? params.call("/control/learn/cancel") : root.learn("any")
            }
            MenuButton {
                width: 112; height: Theme.touch; fontSize: 14
                label: "入力する"
                onTapped: Sheets.text.open("キー（例 ctrl+shift+f9）か MIDI（例 midi:cc/1/7）", trigger_.value,
                                           t => trigger_.set(Bindings.normalizeTrigger(t)))
            }
        }
    }
    Rectangle {
        objectName: "learnBanner"
        visible: root.learning
        width: parent.width
        height: learnText.implicitHeight + 20
        radius: Theme.radius
        color: Theme.accentDim
        Text {
            id: learnText
            anchors { left: parent.left; right: parent.right; verticalCenter: parent.verticalCenter; margins: 12 }
            wrapMode: Text.Wrap
            color: "white"
            font.pixelSize: 15
            text: learnSource.value === "midi" ? "MIDI 機器のボタン・フェーダー・つまみを動かしてください"
                : learnSource.value === "key" ? "エンジンの PC でキーの組み合わせを押してください（Esc でやめる）"
                : "エンジンの PC でキーの組み合わせを押すか、MIDI 機器を動かしてください（Esc でやめる。30 秒で終わります）"
        }
    }
    SettingRow {
        visible: root.isMidi
        label: "MIDI 機器"
        note: "決めておくと、ほかの機器の同じボタンでは動かない"
        controlWidth: 230
        MenuButton {
            width: parent.width; height: Theme.touch; fontSize: 14
            label: device_.value || "どの機器でも"
            onTapped: root.pick("MIDI 機器", Bindings.midiDeviceChoices(device_.value), v => device_.set(v))
        }
    }

    SettingSection { text: "動作" }
    SettingRow {
        label: "押したとき"
        controlWidth: 300
        MenuButton {
            objectName: "modeButton"
            width: parent.width; height: Theme.touch; fontSize: 14
            label: Bindings.modeLabel(root.mode)
            onTapped: root.pick("動作", Bindings.modes.map(m => Object.assign({ selected: m.value === root.mode }, m)),
                                v => root.setMode(v))
        }
    }
    // sampler pad
    SettingRow {
        visible: root.mode === "pad"
        label: "パッド"
        controlWidth: 300
        Row {
            spacing: 6
            MenuButton {
                width: 147; height: Theme.touch; fontSize: 14
                label: root.padBank > 0 ? "バンク " + root.padBank : "今のバンク"
                onTapped: root.pick("バンク", Bindings.bankChoices(true, root.padBank), b => root.setPad(b, root.padNum))
            }
            MenuButton {
                width: 147; height: Theme.touch; fontSize: 14
                label: "パッド " + root.padNum
                onTapped: root.pick("パッド", Bindings.padChoices(root.padBank, root.padNum), p => root.setPad(root.padBank, p))
            }
        }
    }
    SettingRow {
        visible: root.mode === "pad_stop"
        label: "止めるバンク"
        controlWidth: 300
        MenuButton {
            width: parent.width; height: Theme.touch; fontSize: 14
            label: target_.value ? "バンク " + target_.value : "全部"
            onTapped: root.pick("止めるバンク", [{ label: "全部", value: "" }].concat(Bindings.bankChoices(false, Number(target_.value))),
                                v => target_.set(v === "" ? "" : String(v)))
        }
    }
    SettingRow {
        visible: root.mode === "bank"
        label: "切り替えるバンク"
        controlWidth: 300
        MenuButton {
            width: parent.width; height: Theme.touch; fontSize: 14
            label: "バンク " + value_.value
            onTapped: root.pick("バンク", Bindings.bankChoices(false, Number(value_.value)), v => value_.set(String(v)))
        }
    }
    // scene switching and TAKE
    SettingRow {
        visible: root.mode === "scene" || root.mode === "take"
        label: "映像ミックス"
        controlWidth: 300
        MenuButton {
            width: parent.width; height: Theme.touch; fontSize: 14
            label: Video.mixName(root.mode === "scene" ? root.sceneMix : Number(target_.value))
            onTapped: root.pick("映像ミックス", Video.mixChoices(false), m =>
                                target_.set(root.mode === "scene" ? "/video/mix/" + m + "/program_scene" : String(m)))
        }
    }
    SettingRow {
        visible: root.mode === "scene"
        label: "シーン"
        controlWidth: 300
        MenuButton {
            width: parent.width; height: Theme.touch; fontSize: 14
            label: Video.sceneName(Number(value_.value))
            onTapped: root.pick("シーン", Bindings.sceneChoices(Number(value_.value)), v => value_.set(String(v)))
        }
    }
    SettingRow {
        visible: root.mode === "snapshot"
        label: "スナップショット"
        controlWidth: 300
        MenuButton {
            width: parent.width; height: Theme.touch; fontSize: 14
            label: Bindings.snapshotName(Number(target_.value))
            onTapped: root.pick("スナップショット", Bindings.snapshotChoices(Number(target_.value)), v => target_.set(String(v)))
        }
    }
    // a parameter
    SettingRow {
        visible: root.mode === "toggle" || root.mode === "step" || root.mode === "param" || root.mode === "set"
        label: "対象"
        note: root.mode === "set" ? target_.value : ""
        controlWidth: 300
        MenuButton {
            objectName: "targetButton"
            width: parent.width; height: Theme.touch; fontSize: 14
            label: Bindings.targetText(target_.value)
            onTapped: root.pickTarget()
        }
    }
    SettingRow {
        visible: root.mode === "step"
        label: "1 回に変える量"
        note: "マイナスで下げる。音量なら dB"
        controlWidth: 160
        MenuButton {
            width: parent.width; height: Theme.touch; fontSize: 14
            label: (Number(value_.value) > 0 ? "+" : "") + (value_.value || "0")
            onTapped: Sheets.number.open({ title: "1 回に変える量", value: Number(value_.value) || 0, unit: "", min: -100, max: 100, decimals: 2 },
                                         v => value_.set(String(v)))
        }
    }
    SettingRow {
        visible: root.mode === "set"
        label: "値"
        note: "true / false、数値、選択肢の名前"
        ParamText { path: root.base ? root.base + "/value" : ""; title: "値"; emptyText: "（空）" }
    }
    Text {
        visible: root.mode === "param" && !root.isMidi && trigger_.value !== ""
        width: parent.width
        wrapMode: Text.Wrap
        color: Theme.warn
        font.pixelSize: 13
        text: "フェーダー・つまみで動かすのは MIDI だけです。キーには「値の上げ下げ」を使ってください。"
    }

    SettingSection { text: "状態" }
    SettingRow {
        label: "有効"
        controlWidth: 100
        ParamSwitch { path: root.base ? root.base + "/enabled" : "" }
    }
    InfoRow {
        label: "状態"
        value: Bindings.stateText(state_.value)
        valueColor: Bindings.stateColor(state_.value)
        note: state_.value === "invalid" || state_.value === "conflict" ? error_.value : ""
    }

    Item { width: 1; height: 8 }
    MenuButton {
        objectName: "removeBinding"
        width: parent.width
        label: "この割り当てを消す"
        onTapped: {
            const n = root.num
            Sheets.choice.open("割り当て " + n + " を消しますか", [{ label: "消す", value: true }, { label: "やめる", value: false }],
                               c => { if (c.value) { params.call("/control/binding/remove", { binding: n }); root.close() } })
        }
    }
}
