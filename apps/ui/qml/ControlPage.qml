import QtQuick
import DcUi.Params

// Hotkeys and MIDI (protocol.md 5.9): the bindings with their state, the MIDI inputs of the engine PC
// (each can be left to other applications) and the last MIDI message, to check what a control sends.
Item {
    id: page
    readonly property bool swipeable: true

    ParamList { id: bindings; prefix: "/control/binding" }
    ParamList { id: midiDevices; prefix: "/control/midi/device" }
    Param { id: lastTrigger; path: "/control/midi/last/trigger" }
    Param { id: lastDevice; path: "/control/midi/last/device" }
    Param { id: lastValue; path: "/control/midi/last/value" }

    function openDemo(name) {
        // "binding" opens the first one, "binding3" the third
        if (name.startsWith("binding")) bindingSheet.open(Number(name.substring(7)) || (bindings.count > 0 ? bindings.numAt(0) : 1))
        else if (name === "learn") {
            const n = bindings.count > 0 ? bindings.numAt(0) : 1
            bindingSheet.open(n)
            bindingSheet.learn("any")
        }
    }
    function addBinding() {
        params.call("/control/binding/add", { action: "record_toggle" }, (ok, v) => { if (ok) bindingSheet.open(Number(v)) })
    }

    Flickable {
        id: flick
        objectName: "controlFlick"
        anchors.fill: parent
        contentHeight: column.height + 32
        boundsBehavior: Flickable.StopAtBounds
        clip: true

        Column {
            id: column
            x: 24
            width: Math.min(flick.width - 48, 900)

            SettingSection { text: "ホットキーと MIDI の割り当て" }
            Text {
                width: parent.width
                wrapMode: Text.Wrap
                color: Theme.subText
                font.pixelSize: 13
                text: "キーはエンジンの PC のキーボードで、ほかのアプリやゲームを使っている間も効きます。MIDI はエンジンの PC につないだ機器です。割り当てはエンジンの PC に保存されます。"
            }
            Item { width: 1; height: 8 }
            MenuButton {
                objectName: "addBinding"
                width: 240
                height: Theme.touch
                label: "＋ 割り当てを追加"
                onTapped: page.addBinding()
            }
            Item { width: 1; height: 8 }
            Text {
                visible: bindings.count === 0
                color: Theme.subText
                font.pixelSize: 14
                text: "まだ割り当てがありません"
            }
            Repeater {
                model: bindings
                delegate: Rectangle {
                    id: row
                    required property int num
                    required property string base
                    objectName: "binding_" + num
                    Param { id: bTrigger; path: row.base + "/trigger" }
                    Param { id: bAction; path: row.base + "/action" }
                    Param { id: bTarget; path: row.base + "/target" }
                    Param { id: bValue; path: row.base + "/value" }
                    Param { id: bState; path: row.base + "/state" }
                    readonly property bool narrow: width < 640
                    width: column.width
                    height: narrow ? 76 : 52
                    color: tap.pressed ? Theme.controlPressed : (num % 2 ? Theme.stripA : Theme.stripB)
                    radius: Theme.radius
                    Text {
                        id: triggerText
                        x: 12
                        y: row.narrow ? 8 : (row.height - height) / 2
                        width: row.narrow ? row.width - stateText.width - 36 : 250
                        elide: Text.ElideRight
                        color: bTrigger.value ? Theme.text : Theme.subText
                        font.pixelSize: 15
                        font.bold: true
                        text: Bindings.triggerText(bTrigger.value)
                    }
                    Text {
                        x: row.narrow ? 12 : 274
                        y: row.narrow ? 42 : (row.height - height) / 2
                        width: row.narrow ? row.width - 24 : row.width - x - stateText.width - 24
                        elide: Text.ElideRight
                        color: Theme.text
                        font.pixelSize: 14
                        text: Bindings.actionText(bAction.value, bTarget.value, bValue.value)
                    }
                    Text {
                        id: stateText
                        anchors { right: parent.right; rightMargin: 12 }
                        y: row.narrow ? 8 : (row.height - height) / 2
                        color: Bindings.stateColor(bState.value)
                        font.pixelSize: 13
                        text: Bindings.stateText(bState.value)
                    }
                    TapHandler { id: tap; onTapped: bindingSheet.open(row.num) }
                }
            }

            SettingSection { text: "MIDI 機器（エンジンの PC）" }
            Text {
                width: parent.width
                wrapMode: Text.Wrap
                color: Theme.subText
                font.pixelSize: 13
                text: "Windows では 1 つの MIDI 機器を同時に 1 つのアプリしか使えません。DAW などで使う機器はオフにしてください。"
            }
            Text {
                visible: midiDevices.count === 0
                topPadding: 8
                color: Theme.subText
                font.pixelSize: 14
                text: "MIDI 機器が見つかりません（つなぐと数秒で出ます）"
            }
            Repeater {
                model: midiDevices
                delegate: SettingRow {
                    id: dev
                    required property string base
                    Param { id: dName; path: dev.base + "/name" }
                    Param { id: dState; path: dev.base + "/state" }
                    label: dName.value
                    note: Bindings.midiStateText(dState.value)
                    controlWidth: 100
                    ParamSwitch { path: dev.base + "/enabled" }
                }
            }
            InfoRow {
                label: "最後に受けた MIDI"
                value: lastTrigger.value ? Bindings.triggerText(lastTrigger.value) + " = " + lastValue.value + "　" + lastDevice.value : "—"
            }
        }
    }

    BindingSheet { id: bindingSheet; objectName: "bindingSheet" }
}
