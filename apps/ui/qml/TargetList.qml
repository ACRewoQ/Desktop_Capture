import QtQuick
import DcUi.Params

// The used slots of an output or send list (base "/track/3/out", "/track/3/send", "/master/out";
// up to 8 each, an unused slot has target ""). Each row picks its target; sends also have the level
// and "フェーダー前". "+" fills the first unused slot.
Column {
    id: root
    property string base
    property string use: "out"    // out / send / master (RouteChoices.targetChoices)
    property int trackId: 0
    property int slots: 8
    readonly property bool sends: use === "send"

    function pickTarget(slot, title) {
        const path = root.base + "/" + slot + "/target"
        const now = params.value(path)
        const list = routes.targetChoices(root.use, root.trackId)
                           .map(c => Object.assign({ selected: c.value === now && c.header === undefined }, c))
        Sheets.choice.open(title, list, c => params.set(path, c.value))
    }
    function addTarget() {
        for (let i = 1; i <= root.slots; ++i)
            if (params.value(root.base + "/" + i + "/target") === "") {
                pickTarget(i, sends ? "センドを追加" : "出力先を追加")
                return
            }
        Sheets.toast.show((sends ? "センド" : "出力先") + "は " + root.slots + " 個までです")
    }

    width: parent ? parent.width : 0
    spacing: 6

    Repeater {
        model: root.slots
        delegate: Item {
            id: slotItem
            required property int index
            readonly property int slot: index + 1
            readonly property string slotBase: root.base + "/" + slot
            Param { id: target; path: slotItem.slotBase + "/target" }
            visible: target.value !== ""
            width: root.width
            height: visible ? Theme.touch : 0

            Row {
                anchors.fill: parent
                spacing: 6
                Rectangle {
                    objectName: "target_" + slotItem.slot
                    width: parent.width - (root.sends ? 96 + 72 + 12 : 0) - 50
                    height: Theme.touch
                    radius: Theme.radius
                    color: tap.pressed ? Theme.controlPressed : Theme.control
                    Text {
                        anchors { left: parent.left; right: parent.right; margins: 12; verticalCenter: parent.verticalCenter }
                        elide: Text.ElideRight
                        color: "white"
                        font.pixelSize: 15
                        text: target.value, routes.targetText(target.value)
                    }
                    TapHandler {
                        id: tap
                        onTapped: root.pickTarget(slotItem.slot, (root.sends ? "センド " : "出力先 ") + slotItem.slot)
                    }
                }
                ParamNumber {
                    visible: root.sends
                    width: 96
                    path: root.sends ? slotItem.slotBase + "/level_db" : ""
                    title: "センド量"
                    unit: "dB"
                    format: v => (v <= -60 ? "-∞" : (v > 0 ? "+" : "") + Number(v).toFixed(1)) + " dB"
                }
                ToggleButton {
                    visible: root.sends
                    width: 72
                    label: "前"
                    on: pre.value === true
                    onTapped: pre.set(!pre.value)
                    Param { id: pre; path: root.sends ? slotItem.slotBase + "/pre_fader" : "" }
                }
                MenuButton {
                    objectName: "removeTarget_" + slotItem.slot
                    width: 44
                    height: Theme.touch
                    label: "✕"
                    onTapped: target.set("")
                }
            }
        }
    }

    MenuButton {
        objectName: "addTarget"
        width: root.width
        height: Theme.touch
        label: root.sends ? "+ センドを追加" : "+ 出力先を追加"
        onTapped: root.addTarget()
    }
}
