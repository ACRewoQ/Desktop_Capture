import QtQuick
import DcUi.Params

// Where the recordings (target "capture") or the per-track takes ("recorder") are saved: the folder
// setting (/capture/directory, /recorder/directory, an absolute path on the engine's PC, any drive),
// "参照…" to pick it with the system's folder dialog, and below them what /system/folder/check says
// about the folder (free space, or why it will not work). Only the engine's PC can change the folder;
// a remote controller sees it read-only.
Column {
    id: root
    property string target: "capture"
    property string label: "保存先フォルダー"
    property string defaultName: "ビデオフォルダー"
    property var check: null // the answer of /system/folder/check, null until there is one
    readonly property bool local_: !connection.remote

    width: parent ? parent.width : 0
    spacing: 0

    function refresh() {
        if (!connection.ready) return // asked again once connected
        params.call("/system/folder/check", { "for": target }, (ok, v) => {
            if (!ok) { check = null; return } // an engine without the check: show nothing
            try { check = JSON.parse(v) } catch (e) { check = null }
        })
    }
    function sizeText(bytes) {
        const gb = Number(bytes) / 1073741824
        return gb >= 10 ? Math.round(gb) + " GB" : gb >= 1 ? gb.toFixed(1) + " GB" : Math.round(Number(bytes) / 1048576) + " MB"
    }
    readonly property bool lowSpace: !!check && check.free_bytes !== undefined && Number(check.free_bytes) < 2 * 1073741824
    readonly property string statusText: {
        const c = check
        if (!c) return ""
        switch (c.problem) {
        case "not_absolute": return "C:\\… や D:\\… のような、ドライブから始まるパスで入れてください"
        case "no_drive": return "ドライブが見つかりません。USB や外付けのドライブなら、つないでください"
        case "not_folder": return "同じ名前のファイルがあるため、フォルダーにできません"
        case "not_writable": return "このフォルダーには書き込めません"
        }
        const free = c.free_bytes !== undefined ? "空き " + sizeText(c.free_bytes) : ""
        const where = c.default ? root.defaultName + "（" + c.path + "）" : c.path
        return (c.exists ? where : where + "（まだないので、始めるときに作ります）") + (free ? "・" + free : "")
               + (root.lowSpace ? "。空きが少なくなっています" : "")
    }

    Param { id: folder; path: "/" + root.target + "/directory"; onValueChanged: root.refresh() }
    Component.onCompleted: refresh()
    onVisibleChanged: if (visible) refresh()
    Connections { target: connection; function onStateChanged() { if (connection.ready) root.refresh() } }

    SettingRow {
        id: row
        label: root.label
        note: root.local_ ? "このPCの絶対パス。別のドライブも選べます。空なら" + root.defaultName
                          : "エンジンの PC でだけ変えられます"
        controlWidth: root.local_ ? 380 : 300
        Item {
            width: parent ? parent.width : 0
            height: Theme.touch
            ParamText {
                objectName: root.target + "Directory"
                anchors { left: parent.left; right: browse.visible ? browse.left : parent.right; rightMargin: browse.visible ? 8 : 0 }
                path: "/" + root.target + "/directory"
                title: root.label + "（絶対パス）"
                emptyText: root.defaultName
            }
            MenuButton {
                id: browse
                objectName: root.target + "DirectoryBrowse"
                anchors.right: parent.right
                width: 80; height: Theme.touch; fontSize: 14
                visible: root.local_
                label: "参照…"
                onTapped: picker.active = true
            }
        }
    }
    Text {
        objectName: root.target + "DirectoryStatus"
        width: parent.width
        visible: text !== ""
        topPadding: 4
        bottomPadding: 8
        wrapMode: Text.Wrap
        font.pixelSize: 13
        color: !root.check || (root.check.problem === "" && !root.lowSpace) ? Theme.subText : Theme.warn
        text: root.statusText
    }

    // The folder dialog only on the engine's PC, and only while it is open (QtQuick.Dialogs is loaded then).
    Loader {
        id: picker
        active: false
        source: "FolderPicker.qml"
        onLoaded: {
            item.title = root.label
            item.start = root.check && root.check.exists ? root.check.path : ""
            item.picked.connect(p => folder.set(p))
            item.finished.connect(() => picker.active = false)
            item.open()
        }
    }
}
