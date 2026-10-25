import QtQuick
import DcUi.Params

// Width and height parameters (prefix + "/width", "/height") set together from a list of sizes, or
// typed in. presets: [{label?, w, h}].
Rectangle {
    id: root
    property string prefix
    property string title: "解像度"
    property var presets: [{ w: 3840, h: 2160 }, { w: 2560, h: 1440 }, { w: 1920, h: 1080 }, { w: 1280, h: 720 },
                           { w: 1080, h: 1920, label: "1080×1920（縦）" }, { w: 960, h: 540 }, { w: 640, h: 360 }]

    function setSize(w, h) {
        const ch = {}
        ch[prefix + "/width"] = w
        ch[prefix + "/height"] = h
        params.setMany(ch)
    }

    width: parent ? parent.width : 0
    height: Theme.touch
    radius: Theme.radius
    color: tap.pressed ? Theme.controlPressed : Theme.control
    Param { id: w; path: root.prefix ? root.prefix + "/width" : "" }
    Param { id: h; path: root.prefix ? root.prefix + "/height" : "" }

    Text {
        anchors.centerIn: parent
        color: "white"
        font.pixelSize: 15
        text: w.value + " × " + h.value
    }
    TapHandler {
        id: tap
        onTapped: {
            const list = root.presets.filter(p => p.w >= w.min && p.w <= w.max && p.h >= h.min && p.h <= h.max)
                .map(p => ({ label: p.label || p.w + "×" + p.h, value: p, selected: p.w === w.value && p.h === h.value }))
            list.push({ label: "数値で入力…", value: null })
            Sheets.choice.open(root.title, list, c => {
                if (c.value) { root.setSize(c.value.w, c.value.h); return }
                Sheets.number.open({ title: "幅", value: w.value, unit: "px", min: w.min, max: w.max, decimals: 0 }, nw =>
                    Sheets.number.open({ title: "高さ", value: h.value, unit: "px", min: h.min, max: h.max, decimals: 0 },
                                       nh => root.setSize(nw, nh)))
            })
        }
    }
}
