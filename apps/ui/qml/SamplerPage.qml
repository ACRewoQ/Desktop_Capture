import QtQuick
import DcUi.Params

// Sampler pads (8 banks x 16 pads). A pad sounds while it is pressed (mode decides what a press does);
// several fingers can play several pads. "編集" turns taps into opening the pad's settings.
Item {
    id: page
    readonly property bool swipeable: false // fingers slide across pads while playing
    property bool editing: false

    Param { id: activeBank; path: "/sampler/active_bank" }
    readonly property int bank: activeBank.value > 0 ? activeBank.value : 1
    Param { id: bankName; path: "/sampler/bank/" + page.bank + "/name" }

    function openDemo(name) {
        if (name === "pad") padSheet.open(page.bank, 1)
        else if (name === "edit") editing = true
        else if (name === "library") { padSheet.open(page.bank, 1); librarySheet.open(page.bank, 1) }
    }

    // Narrow screens put the stop / edit buttons on a second row.
    readonly property bool narrow: width < 900

    Row {
        id: banks
        anchors { top: parent.top; left: parent.left; margins: 12 }
        spacing: 4
        Repeater {
            model: 8
            delegate: Rectangle {
                id: bankTab
                required property int index
                readonly property int num: index + 1
                readonly property bool current: page.bank === num
                objectName: "bank_" + num
                width: Math.min(96, ((page.narrow ? page.width - 24 : tools.x - 24) + 4) / 8 - 4)
                height: Theme.touch
                radius: Theme.radius
                color: current ? Theme.accentDim : tabTap.pressed ? Theme.controlPressed : Theme.control
                border.width: current ? 1 : 0
                border.color: Theme.accent
                Param { id: tabName; path: "/sampler/bank/" + bankTab.num + "/name" }
                Text {
                    anchors.centerIn: parent
                    width: parent.width - 6
                    horizontalAlignment: Text.AlignHCenter
                    elide: Text.ElideRight
                    color: "white"
                    font.pixelSize: 14
                    text: tabName.value || "B" + bankTab.num
                }
                TapHandler { id: tabTap; onTapped: activeBank.set(bankTab.num) }
            }
        }
    }
    Row {
        id: tools
        anchors { top: page.narrow ? banks.bottom : parent.top; right: parent.right; margins: 12; topMargin: page.narrow ? 8 : 12 }
        spacing: 6
        MenuButton {
            objectName: "stopAll"
            width: 120; height: Theme.touch; fontSize: 14
            label: "■ 全部止める"
            onTapped: params.call("/sampler/stop")
        }
        ToggleButton {
            objectName: "editPads"
            width: 80
            fontSize: 14
            label: "編集"
            on: page.editing
            onTapped: page.editing = !page.editing
        }
    }

    Item {
        id: editBar
        anchors { top: page.narrow ? tools.bottom : banks.bottom; topMargin: 8; left: parent.left; right: parent.right; leftMargin: 12; rightMargin: 12 }
        height: page.editing ? Theme.touch : 0
        visible: page.editing
        Text {
            anchors { left: parent.left; verticalCenter: parent.verticalCenter }
            color: Theme.warn
            font.pixelSize: 14
            text: "編集中: パッドをタップすると設定を開きます"
        }
        MenuButton {
            anchors { right: parent.right; verticalCenter: parent.verticalCenter }
            width: 200; height: Theme.touch; fontSize: 14
            label: "バンク名: " + (bankName.value || "B" + page.bank)
            onTapped: Sheets.text.open("バンク " + page.bank + " の名前", bankName.value, t => bankName.set(t))
        }
    }

    Grid {
        id: grid
        objectName: "padGrid"
        anchors { top: editBar.bottom; topMargin: 8; left: parent.left; right: parent.right; bottom: parent.bottom; margins: 12 }
        columns: 4
        spacing: 8
        readonly property real cellWidth: (width - spacing * 3) / 4
        readonly property real cellHeight: (height - spacing * 3) / 4
        Repeater {
            model: 16
            delegate: Rectangle {
                id: pad
                required property int index
                readonly property int num: index + 1
                readonly property string base: "/sampler/bank/" + page.bank + "/pad/" + num
                objectName: "pad_" + num
                width: grid.cellWidth
                height: grid.cellHeight
                radius: 8
                Param { id: pFile; path: pad.base + "/file" }
                Param { id: pName; path: pad.base + "/name" }
                Param { id: pColor; path: pad.base + "/color" }
                Param { id: pMode; path: pad.base + "/mode" }
                Param { id: pState; path: pad.base + "/state" }
                Param { id: pPlaying; path: pad.base + "/playing" }
                readonly property color tint: pColor.value ? pColor.value : "#3f5a73"
                readonly property bool empty: pState.value === "empty" || !pFile.value
                color: empty ? Theme.stripA : Qt.darker(tint, finger.active ? 1.0 : pPlaying.value ? 1.15 : 1.6)
                border.width: pPlaying.value ? 3 : 1
                border.color: pPlaying.value ? "white" : empty ? Theme.border : tint

                Text {
                    anchors { left: parent.left; right: parent.right; top: parent.top; margins: 8 }
                    color: Theme.subText
                    font.pixelSize: 12
                    text: pad.num + (pad.empty ? "" : "  " + ({ oneshot: "", toggle: "押すたび", hold: "押す間", loop: "ループ" })[pMode.value])
                }
                Text {
                    anchors.centerIn: parent
                    width: parent.width - 12
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    maximumLineCount: 3
                    elide: Text.ElideRight
                    color: pad.empty ? Theme.subText : "white"
                    font.pixelSize: Math.max(12, Math.min(18, pad.height / 6))
                    text: pad.empty ? (page.editing ? "+" : "")
                        : pState.value === "loading" ? "読み込み中…"
                        : pState.value === "error" ? "読めません"
                        : pName.value || String(pFile.value).replace(/^.*[\\\/]/, "").replace(/\.[^.]*$/, "")
                }

                // Press and release for each finger separately (hold pads stop on release).
                PointHandler {
                    id: finger
                    enabled: !page.editing
                    onActiveChanged: {
                        if (pad.empty) return
                        if (active) params.call("/sampler/trigger", { pad: pad.num, bank: page.bank })
                        else if (pMode.value === "hold") params.call("/sampler/release", { pad: pad.num, bank: page.bank })
                    }
                }
                TapHandler {
                    enabled: page.editing
                    onTapped: padSheet.open(page.bank, pad.num)
                }
            }
        }
    }

    PadSheet { id: padSheet; objectName: "padSheet"; onLibraryWanted: (b, p) => librarySheet.open(b, p) }
    LibrarySheet { id: librarySheet; objectName: "librarySheet" }
}
