import QtQuick
import DcUi.Params

// The effect chain of a track or the master (base "/track/3" or "/master"): order, on / off, add,
// remove; a tap on an effect opens its parameters.
Sheet {
    id: root
    property string base

    signal editRequested(string slot)

    function open(stripBase, name) {
        base = stripBase
        title = name + " のエフェクト"
        shown = true
    }
    function addChoices() {
        const list = params.builtinEffects().map(e => ({ label: e.name, value: e.type }))
        list.push({ header: "VST3 プラグイン" })
        let any = false
        for (let i = 0; i < plugins.count; ++i) {
            const b = "/plugins/" + plugins.numAt(i)
            if (params.value(b + "/instrument")) continue
            const st = params.value(b + "/status")
            any = true
            list.push({ label: params.value(b + "/name"),
                        detail: st === "ok" ? params.value(b + "/vendor")
                                            : (st === "unsupported" ? "モノ・ステレオに対応していません" : "読み込めません: " + params.value(b + "/error")),
                        value: params.value(b + "/id"), disabled: st !== "ok" })
        }
        if (!any) list.push({ label: "プラグインがありません", detail: "設定ページでスキャンできます", disabled: true })
        return list
    }

    panelWidth: 560
    onClosed: base = ""

    ParamList { id: plugins; prefix: "/plugins" }
    ParamList { id: slots; prefix: root.base ? root.base + "/fx" : "" }

    Text {
        visible: slots.count === 0
        width: parent.width
        color: Theme.subText
        font.pixelSize: 14
        text: "エフェクトはありません。上から順に処理されます（ローカット・ハイカットの後、フェーダーの前）。"
        wrapMode: Text.Wrap
    }

    Repeater {
        model: slots
        delegate: Item {
            id: fxRow
            required property int num
            required property string base
            required property int index
            Param { id: fxName; path: fxRow.base + "/name" }
            Param { id: fxStatus; path: fxRow.base + "/status" }
            Param { id: fxBypass; path: fxRow.base + "/bypass" }
            Param { id: fxType; path: fxRow.base + "/type" }
            width: parent ? parent.width : 0
            height: 52

            Row {
                anchors.fill: parent
                spacing: 6
                Rectangle {
                    objectName: "fxSlot_" + fxRow.num
                    width: parent.width - 4 * 6 - 72 - 3 * 44
                    height: parent.height
                    radius: Theme.radius
                    color: slotTap.pressed ? Theme.controlPressed : Theme.control
                    opacity: fxBypass.value ? 0.55 : 1
                    Column {
                        anchors { left: parent.left; right: parent.right; margins: 12; verticalCenter: parent.verticalCenter }
                        Text {
                            width: parent.width
                            elide: Text.ElideRight
                            color: "white"
                            font.pixelSize: 15
                            text: fxRow.num + ". " + fxName.value
                        }
                        Text {
                            width: parent.width
                            elide: Text.ElideRight
                            font.pixelSize: 11
                            color: fxStatus.value === "ok" ? Theme.subText : fxStatus.value === "loading" ? Theme.warn : Theme.mute
                            text: fxStatus.value === "ok" ? (String(fxType.value).startsWith("vst3:") ? "VST3" : "内蔵")
                                : fxStatus.value === "loading" ? "読み込み中…"
                                : fxStatus.value === "missing" ? "プラグインがありません" : "読み込み失敗"
                        }
                    }
                    TapHandler { id: slotTap; onTapped: root.editRequested(fxRow.base) }
                }
                ToggleButton {
                    objectName: "fxOn_" + fxRow.num
                    width: 72
                    height: parent.height
                    label: fxBypass.value ? "オフ" : "オン"
                    fontSize: 14
                    on: !fxBypass.value
                    onTapped: fxBypass.set(!fxBypass.value)
                }
                MenuButton {
                    width: 44; height: parent.height
                    label: "↑"
                    active: fxRow.index > 0
                    onTapped: params.call(root.base + "/fx/move", { from: fxRow.num, to: fxRow.num - 1 })
                }
                MenuButton {
                    width: 44; height: parent.height
                    label: "↓"
                    active: fxRow.index < slots.count - 1
                    onTapped: params.call(root.base + "/fx/move", { from: fxRow.num, to: fxRow.num + 1 })
                }
                MenuButton {
                    objectName: "fxRemove_" + fxRow.num
                    width: 44; height: parent.height
                    label: "✕"
                    onTapped: {
                        const n = fxRow.num
                        const b = root.base
                        Sheets.choice.open(fxName.value + " を削除しますか",
                                           [{ label: "削除する", value: true }, { label: "やめる", value: false }],
                                           c => { if (c.value) params.call(b + "/fx/remove", { index: n }) })
                    }
                }
            }
        }
    }

    MenuButton {
        objectName: "addFx"
        width: parent.width
        height: Theme.touch
        label: "+ エフェクトを追加"
        active: slots.count < 16
        onTapped: {
            const b = root.base
            Sheets.choice.open("エフェクトを追加", root.addChoices(),
                               c => params.call(b + "/fx/add", { type: c.value }))
        }
    }
}
