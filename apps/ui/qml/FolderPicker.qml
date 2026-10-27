import QtQuick
import QtQuick.Dialogs

// The system's folder dialog for SaveFolderRow (loaded only while it is open, on the engine's PC).
// picked() gets a local path: D:\Videos, \\server\share\Videos (or /home/... off Windows).
FolderDialog {
    id: dialog
    property string start: ""
    signal picked(string path)
    signal finished()

    function toUrl(path) {
        if (path === "") return ""
        const p = path.replace(/\\/g, "/")
        return p.startsWith("//") ? "file:" + encodeURI(p) : "file:///" + encodeURI(p.replace(/^\//, ""))
    }
    function toPath(url) {
        let u = decodeURIComponent(String(url))
        if (u.startsWith("file:///")) {
            u = u.substring(8)
            return /^[A-Za-z]:/.test(u) ? u.replace(/\//g, "\\") : "/" + u
        }
        if (u.startsWith("file://")) return "\\\\" + u.substring(7).replace(/\//g, "\\") // a network share
        return u
    }

    currentFolder: toUrl(start)
    onAccepted: {
        picked(toPath(selectedFolder))
        finished()
    }
    onRejected: finished()
}
