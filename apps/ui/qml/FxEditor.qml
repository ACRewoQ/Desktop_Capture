import QtQuick
import DcUi.Params

// One effect's parameters. The list (names, ranges, display texts) comes from the engine's
// /fx/#/params; values follow the tree. VST3 plugins can have thousands of parameters: they come in
// pages of 128 and can be filtered by name; the plugin's own window opens on the engine's PC.
Sheet {
    id: root
    property string slot              // "/track/3/fx/2"
    property var infos: []
    property int loaded: 0
    property bool loading: false
    property string filter: ""
    readonly property bool vst: String(type.value).startsWith("vst3:")
    readonly property int pageSize: 128

    function open(slotBase) {
        slot = slotBase
        infos = []
        loaded = 0
        filter = ""
        shown = true
        fetch()
    }
    function fetch() {
        if (loading) return
        loading = true
        params.call(slot + "/params", { offset: loaded, count: pageSize }, (ok, json) => {
            loading = false
            if (!ok) return
            const list = JSON.parse(json)
            infos = infos.concat(list)
            loaded += list.length
        })
    }
    // New display texts (VST3 decides them) after a change.
    function refreshText(key) {
        params.call(slot + "/params", { keys: key }, (ok, json) => {
            if (!ok) return
            const fresh = JSON.parse(json)
            if (!fresh.length) return
            const copy = infos.slice()
            for (let i = 0; i < copy.length; ++i)
                if (copy[i].key === key) copy[i] = fresh[0]
            infos = copy
        })
    }
    function valueText(info, v) {
        if (root.vst) return info.text
        if (info.steps === 2) return v >= 0.5 ? "オン" : "オフ"
        const u = info.unit
        if (u === "Hz") return v >= 1000 ? (v / 1000).toFixed(v >= 10000 ? 1 : 2) + " kHz" : Math.round(v) + " Hz"
        if (u === "ms") return (v < 10 ? v.toFixed(1) : Math.round(v)) + " ms"
        if (u === "dB") return (v > 0 ? "+" : "") + v.toFixed(1) + " dB"
        if (u === ":1") return v.toFixed(1) + ":1"
        if (info.max <= 1 && info.min >= 0) return Math.round(v * 100) + " %"
        return v.toFixed(2) + (u ? " " + u : "")
    }

    panelWidth: 640
    title: name.value || "エフェクト"
    onClosed: { infos = []; slot = "" }

    Param { id: type; path: root.slot ? root.slot + "/type" : "" }
    Param { id: name; path: root.slot ? root.slot + "/name" : "" }
    Param {
        id: status
        path: root.slot ? root.slot + "/status" : ""
        // A VST3 that was still loading when the editor opened: its parameters arrive now.
        onValueChanged: if (root.shown && value === "ok" && root.loaded === 0) root.fetch()
    }
    Param { id: error; path: root.slot ? root.slot + "/error" : "" }
    Param { id: latency; path: root.slot ? root.slot + "/latency_samples" : "" }
    Param { id: count; path: root.slot ? root.slot + "/param_count" : "" }

    Text {
        width: parent.width
        visible: status.value !== "ok"
        wrapMode: Text.Wrap
        color: status.value === "loading" ? Theme.warn : Theme.mute
        font.pixelSize: 14
        text: status.value === "loading" ? "読み込み中…"
            : status.value === "missing" ? "このプラグインはエンジンの PC にありません"
            : "読み込めませんでした: " + error.value
    }
    SettingRow {
        label: "有効"
        note: latency.value > 0 ? "遅延 " + (latency.value / 48).toFixed(1) + " ms（" + latency.value + " サンプル）" : ""
        controlWidth: 100
        Item {
            width: parent.width
            height: Theme.touch
            Param { id: bypass; path: root.slot ? root.slot + "/bypass" : "" }
            ToggleButton {
                objectName: "fxEnabled"
                anchors.right: parent.right
                width: 88
                label: bypass.value ? "オフ" : "オン"
                on: !bypass.value
                onTapped: bypass.set(!bypass.value)
            }
        }
    }
    SettingRow {
        visible: root.vst
        label: "プラグインの画面"
        note: "エンジンの PC の画面に開きます"
        controlWidth: 100
        ParamSwitch { path: root.vst ? root.slot + "/editor" : "" }
    }

    // Filter (VST3 only).
    Rectangle {
        visible: root.vst && count.value > 16
        width: parent.width
        height: Theme.touch
        radius: Theme.radius
        color: Theme.background
        border.color: filterInput.activeFocus ? Theme.accent : Theme.border
        TextInput {
            id: filterInput
            anchors { fill: parent; leftMargin: 12; rightMargin: 12 }
            verticalAlignment: TextInput.AlignVCenter
            color: Theme.text
            font.pixelSize: 15
            clip: true
            onTextChanged: root.filter = text.toLowerCase()
            Text {
                visible: !parent.text && !parent.activeFocus
                anchors.verticalCenter: parent.verticalCenter
                color: Theme.subText
                font.pixelSize: 15
                text: "名前で絞り込む（読み込んだ " + root.loaded + " / " + count.value + " 個から）"
            }
        }
        TapHandler { onTapped: filterInput.forceActiveFocus() }
    }

    Repeater {
        model: root.infos
        delegate: Column {
            id: paramRow
            required property var modelData
            required property int index
            readonly property string path: root.slot + "/param/" + modelData.key
            visible: !root.filter || modelData.name.toLowerCase().indexOf(root.filter) >= 0
            width: parent ? parent.width : 0
            spacing: 0
            objectName: "fxParam_" + modelData.key

            Item {
                width: parent.width
                height: 24
                Text {
                    anchors { left: parent.left; verticalCenter: parent.verticalCenter }
                    width: parent.width - valueLabel.width - 8
                    elide: Text.ElideRight
                    color: Theme.text
                    font.pixelSize: 14
                    text: paramRow.modelData.name
                }
                Rectangle {
                    anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                    width: valueLabel.width
                    height: 24
                    radius: 4
                    color: valueTap.pressed ? Theme.controlPressed : "transparent"
                    Text {
                        id: valueLabel
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        rightPadding: 4
                        leftPadding: 8
                        color: Theme.subText
                        font.pixelSize: 14
                        text: root.valueText(paramRow.modelData, Number(slider.param.value))
                    }
                    // Long press: type the value (VST3 in percent).
                    TapHandler {
                        id: valueTap
                        onLongPressed: {
                            const i = paramRow.modelData
                            if (root.vst)
                                Sheets.number.open({ title: i.name, value: slider.param.value * 100, unit: "%", min: 0, max: 100, decimals: 1 },
                                                   v => { slider.param.set(v / 100); root.refreshText(i.key) })
                            else
                                Sheets.number.open({ title: i.name, value: slider.param.value, unit: i.unit, min: i.min, max: i.max,
                                                     decimals: i.unit === "Hz" ? 0 : 2 }, v => slider.param.set(v))
                        }
                    }
                }
            }
            ParamSlider {
                id: slider
                visible: paramRow.modelData.steps !== 2
                path: paramRow.path
                from: paramRow.modelData.min
                to: paramRow.modelData.max
                log: !!paramRow.modelData.log
                steps: paramRow.modelData.steps
                defaultValue: paramRow.modelData["default"]
                onReleased: if (root.vst) root.refreshText(paramRow.modelData.key)
            }
            ParamSwitch {
                visible: paramRow.modelData.steps === 2
                path: paramRow.modelData.steps === 2 ? paramRow.path : ""
            }
        }
    }

    MenuButton {
        visible: root.loaded < count.value && root.infos.length > 0
        width: parent.width
        height: Theme.touch
        label: root.loading ? "読み込み中…" : "さらに読み込む（あと " + (count.value - root.loaded) + " 個）"
        onTapped: root.fetch()
    }
}
