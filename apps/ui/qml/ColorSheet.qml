import QtQuick

// Colour choice: a palette, the opacity (when alpha) and the hex code (keypad-free: text entry).
// open({title, value, alpha}, callback); value and the result are 0xAARRGGBB (0xRRGGBB without alpha).
Sheet {
    id: root
    property var callback: null
    property bool alpha: true
    property real rgb: 0          // 0xRRGGBB
    property int a: 255
    readonly property var palette: [
        0x000000, 0x404040, 0x808080, 0xc0c0c0, 0xffffff, 0x00ff00,
        0xe53935, 0xfb8c00, 0xfdd835, 0x43a047, 0x00acc1, 0x1e88e5,
        0x3949ab, 0x8e24aa, 0xd81b60, 0x6d4c41, 0x1d2026, 0x0000ff
    ]

    function hex2(n) { return ("0" + n.toString(16)).slice(-2) }
    function cssColor(rgbValue, alphaValue) {
        return "#" + hex2(alphaValue) + ("00000" + Math.round(rgbValue).toString(16)).slice(-6)
    }
    function open(o, cb) {
        title = o.title || "色"
        alpha = o.alpha !== false
        const v = Number(o.value) || 0
        rgb = v % 0x1000000
        a = alpha ? Math.floor(v / 0x1000000) : 255
        callback = cb || null
        shown = true
    }
    function accept() {
        const cb = callback
        const v = alpha ? a * 0x1000000 + rgb : rgb
        close()
        if (cb) cb(v)
    }

    panelWidth: 420
    showClose: false
    onClosed: callback = null

    Rectangle {
        width: parent.width
        height: 56
        radius: Theme.radius
        // Checkerboard-free preview: the colour over the panel and over white.
        color: "white"
        Rectangle { anchors.fill: parent; radius: parent.radius; color: root.cssColor(root.rgb, root.a) }
        Text {
            anchors.centerIn: parent
            color: (root.rgb % 256 + Math.floor(root.rgb / 256) % 256 + Math.floor(root.rgb / 65536)) / 3 > 140 ? "black" : "white"
            font.pixelSize: 16
            text: "#" + ("00000" + Math.round(root.rgb).toString(16)).slice(-6).toUpperCase() +
                  (root.alpha ? "  不透明度 " + Math.round(root.a / 2.55) + " %" : "")
        }
        TapHandler {
            onTapped: Sheets.text.open("色コード（RRGGBB）", ("00000" + Math.round(root.rgb).toString(16)).slice(-6), t => {
                const n = parseInt(t.replace("#", ""), 16)
                if (!isNaN(n)) root.rgb = n % 0x1000000
                root.shown = true
            })
        }
    }
    Grid {
        columns: 6
        spacing: 6
        Repeater {
            model: root.palette
            delegate: Rectangle {
                required property var modelData
                width: (root.contentWidth - 30) / 6
                height: 44
                radius: Theme.radius
                color: root.cssColor(modelData, 255)
                border.width: Math.round(root.rgb) === modelData ? 3 : 1
                border.color: Math.round(root.rgb) === modelData ? Theme.accent : Theme.border
                TapHandler { onTapped: root.rgb = modelData }
            }
        }
    }
    Row {
        visible: root.alpha
        spacing: 4
        Repeater {
            model: [255, 191, 128, 64, 0]
            delegate: ToggleButton {
                required property int modelData
                width: (root.contentWidth - 16) / 5
                label: Math.round(modelData / 2.55) + "%"
                fontSize: 13
                on: root.a === modelData
                onTapped: root.a = modelData
            }
        }
    }
    Row {
        spacing: 8
        MenuButton { width: (root.contentWidth - 8) / 2; label: "やめる"; onTapped: root.close() }
        MenuButton {
            objectName: "colorOk"
            width: (root.contentWidth - 8) / 2
            label: "決定"
            color: Theme.accentDim
            onTapped: root.accept()
        }
    }
}
