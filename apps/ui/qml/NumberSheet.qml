import QtQuick

// Number entry with a touch keypad (long press on a value). open({title, value, unit, min, max,
// decimals}, callback); callback(number) runs with the value clamped to min..max.
Sheet {
    id: root
    property string entry: ""
    property bool fresh: true      // the first key replaces the shown value
    property string unit
    property real minValue: -1e9
    property real maxValue: 1e9
    property int decimals: 1
    property var callback: null

    panelWidth: 340
    showClose: false

    function open(o, cb) {
        title = o.title || ""
        unit = o.unit || ""
        minValue = o.min !== undefined ? o.min : -1e9
        maxValue = o.max !== undefined ? o.max : 1e9
        decimals = o.decimals !== undefined ? o.decimals : 1
        entry = o.value !== undefined ? Number(o.value).toFixed(decimals) : ""
        fresh = true
        callback = cb || null
        shown = true
    }
    function key(k) {
        if (fresh && k !== "⌫" && k !== "±") entry = ""
        fresh = false
        if (k === "⌫") entry = entry.slice(0, -1)
        else if (k === "±") entry = entry.startsWith("-") ? entry.slice(1) : "-" + entry
        else if (k === ".") { if (decimals > 0 && entry.indexOf(".") < 0) entry += entry.length && entry !== "-" ? "." : "0." }
        else entry += k
    }
    function accept() {
        const v = parseFloat(entry)
        const cb = callback
        close()
        if (isNaN(v) || !cb) return
        cb(Math.max(minValue, Math.min(maxValue, v)))
    }

    onClosed: callback = null

    Rectangle {
        width: parent ? parent.width : 0
        height: 52
        radius: Theme.radius
        color: Theme.background
        Text {
            objectName: "numberEntry"
            anchors { right: parent.right; rightMargin: 14; verticalCenter: parent.verticalCenter }
            color: root.fresh ? Theme.subText : Theme.text
            font.pixelSize: 24
            text: (root.entry.length ? root.entry : "0") + (root.unit ? " " + root.unit : "")
        }
    }
    Text {
        width: parent ? parent.width : 0
        horizontalAlignment: Text.AlignHCenter
        color: Theme.subText
        font.pixelSize: 12
        text: root.minValue > -1e9 ? root.minValue + " 〜 " + root.maxValue : ""
    }
    Grid {
        id: keys
        columns: 4
        spacing: 6
        readonly property real keyWidth: (root.contentWidth - 18) / 4
        Repeater {
            model: ["7", "8", "9", "⌫", "4", "5", "6", "±", "1", "2", "3", "."]
            delegate: MenuButton {
                required property string modelData
                width: keys.keyWidth
                height: 56
                label: modelData
                active: modelData !== "." || root.decimals > 0
                onTapped: root.key(modelData)
            }
        }
    }
    // Separate row: in a Grid the wide button would widen its whole column.
    Row {
        spacing: 6
        MenuButton {
            width: keys.keyWidth
            height: 56
            label: "0"
            onTapped: root.key("0")
        }
        MenuButton {
            objectName: "numberOk"
            width: keys.keyWidth * 3 + 12
            height: 56
            label: "決定"
            color: Theme.accentDim
            onTapped: root.accept()
        }
    }
    MenuButton {
        width: parent ? parent.width : 0
        label: "やめる"
        onTapped: root.close()
    }
}
