import QtQuick
import DcUi.Params

// Picks a pad's sound from the sampler library (/sampler/library/folder on the engine's PC), one
// folder level at a time (/sampler/library/list). The folder itself can only be changed on the
// engine's PC; a remote controller sees it read-only but can still pick files from it.
Sheet {
    id: root
    property int bank: 0
    property int pad: 0
    readonly property string base: pad > 0 ? "/sampler/bank/" + bank + "/pad/" + pad : ""
    property string dir: "" // relative to the library, "/" separated; kept between openings
    property var entries: []
    property int total: 0
    property bool loading: false
    property string message: ""
    readonly property int pageSize: 200

    function open(b, p) {
        bank = b
        pad = p
        title = "バンク " + b + " のパッド " + p + " の音"
        shown = true
        reload()
    }
    function reload() { load(dir, 0) }
    function enter(d) { load(d, 0) }
    function up() { enter(dir.indexOf("/") < 0 ? "" : dir.substring(0, dir.lastIndexOf("/"))) }
    function load(d, offset) {
        if (offset === 0) {
            entries = []
            total = 0
        }
        message = ""
        if (!folder.value) {
            dir = ""
            message = connection.remote ? "ライブラリのフォルダーが決まっていません。エンジンの PC のこの画面で決めてください"
                                        : "ライブラリのフォルダーが決まっていません。上で決めてください"
            return
        }
        loading = true
        const asked = d
        params.call("/sampler/library/list", { dir: d, offset: offset, count: pageSize }, (ok, v) => {
            loading = false
            if (!ok) {
                // a folder that went away: back to the top once
                if (asked !== "" && String(v).indexOf("no such folder") >= 0) { load("", 0); return }
                message = String(v).indexOf("library folder not found") >= 0 ? "ライブラリのフォルダーが見つかりません"
                    : String(v).indexOf("absolute") >= 0 ? "ライブラリのフォルダーは C:\\… のような絶対パスで入れてください"
                    : String(v)
                return
            }
            const r = JSON.parse(v)
            dir = r.dir
            entries = offset === 0 ? r.entries : entries.concat(r.entries)
            total = r.total
            if (total === 0) message = "このフォルダーには使える音声ファイルがありません"
            else if (r.truncated) message = "ファイルが多すぎるため、一部だけ表示しています"
        })
    }
    function sizeText(bytes) {
        const mb = Number(bytes || 0) / 1048576
        return mb >= 1 ? mb.toFixed(1) + " MB" : Math.max(1, Math.round(Number(bytes || 0) / 1024)) + " KB"
    }
    function pick(entry) {
        const ch = {}
        ch[base + "/file"] = entry.path
        ch[base + "/name"] = "" // the file name
        params.setMany(ch)
        close()
    }

    panelWidth: 600
    onClosed: pad = 0

    Param { id: folder; path: "/sampler/library/folder"; onValueChanged: if (root.shown) root.load("", 0) }
    Param { id: current; path: root.base ? root.base + "/file" : "" }

    SettingRow {
        label: "ライブラリのフォルダー"
        note: connection.remote ? "エンジンの PC でだけ変えられます" : "エンジンの PC の絶対パス。中のフォルダーも開けます"
        controlWidth: 320
        ParamText { objectName: "libraryFolder"; path: "/sampler/library/folder"; title: "ライブラリのフォルダー（絶対パス）"; emptyText: "未設定" }
    }

    Item {
        width: parent ? parent.width : 0
        height: Theme.touch
        visible: !!folder.value
        MenuButton {
            id: upButton
            objectName: "libraryUp"
            anchors { left: parent.left; verticalCenter: parent.verticalCenter }
            width: 88; height: Theme.touch; fontSize: 14
            label: "↑ 上へ"
            active: root.dir !== ""
            onTapped: root.up()
        }
        Text {
            anchors { left: upButton.right; leftMargin: 10; right: parent.right; verticalCenter: parent.verticalCenter }
            elide: Text.ElideLeft
            color: Theme.subText
            font.pixelSize: 14
            text: root.dir === "" ? "（ライブラリの一番上）" : "/ " + root.dir.split("/").join(" / ")
        }
    }

    Text {
        width: parent ? parent.width : 0
        visible: root.message !== "" || root.loading
        wrapMode: Text.Wrap
        color: root.loading ? Theme.subText : Theme.warn
        font.pixelSize: 14
        text: root.loading ? "読み込み中…" : root.message
    }

    Repeater {
        model: root.entries
        delegate: Rectangle {
            required property var modelData
            required property int index
            readonly property bool selected: !modelData.folder && modelData.path === current.value
            objectName: "libraryEntry_" + index
            width: parent ? parent.width : 0
            height: 48
            radius: Theme.radius
            color: tap.pressed ? Theme.controlPressed : modelData.folder ? Theme.stripB : Theme.control
            border.width: selected ? 2 : 0
            border.color: Theme.accent
            Text {
                anchors { left: parent.left; leftMargin: 12; right: detail.left; rightMargin: 8; verticalCenter: parent.verticalCenter }
                elide: Text.ElideMiddle
                color: modelData.folder ? Theme.accent : "white"
                font.pixelSize: 15
                text: modelData.name
            }
            Text {
                id: detail
                anchors { right: parent.right; rightMargin: 12; verticalCenter: parent.verticalCenter }
                color: Theme.subText
                font.pixelSize: 12
                text: modelData.folder ? "›" : root.sizeText(modelData.size)
            }
            TapHandler {
                id: tap
                onTapped: modelData.folder ? root.enter(root.dir === "" ? modelData.name : root.dir + "/" + modelData.name)
                                           : root.pick(modelData)
            }
        }
    }

    MenuButton {
        objectName: "libraryMore"
        width: parent ? parent.width : 0
        visible: !root.loading && root.entries.length < root.total
        label: "続きを表示（残り " + (root.total - root.entries.length) + "）"
        onTapped: root.load(root.dir, root.entries.length)
    }
}
