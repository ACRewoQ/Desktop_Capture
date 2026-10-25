import QtQuick
import DcUi.Params

// Settings of one layer: source, name, how the source fits, where the box is (presets or numbers),
// opacity, crop, chroma key, what to show without signal, and delete.
Sheet {
    id: root
    property int scene: 0
    property int layerNum: 0
    readonly property string base: layerNum > 0 ? "/video/scene/" + scene + "/layer/" + layerNum : ""

    // Box presets as fractions of the canvas.
    readonly property var presets: [
        { label: "全体", box: [0, 0, 1, 1] },
        { label: "左半分", box: [0, 0, 0.5, 1] },
        { label: "右半分", box: [0.5, 0, 0.5, 1] },
        { label: "左上", box: [0, 0, 0.5, 0.5] },
        { label: "右上", box: [0.5, 0, 0.5, 0.5] },
        { label: "左下", box: [0, 0.5, 0.5, 0.5] },
        { label: "右下", box: [0.5, 0.5, 0.5, 0.5] },
        { label: "ワイプ右下", box: [0.7, 0.7, 0.28, 0.28] },
        { label: "ワイプ左下", box: [0.02, 0.7, 0.28, 0.28] },
        { label: "中央", box: [0.2, 0.2, 0.6, 0.6] },
    ]

    function open(s, l) {
        scene = s
        layerNum = l
        title = Video.layerName(s, l)
        shown = true
    }
    function applyBox(box) {
        const c = {}
        c[base + "/x"] = box[0]
        c[base + "/y"] = box[1]
        c[base + "/width"] = box[2]
        c[base + "/height"] = box[3]
        params.setMany(c)
    }

    panelWidth: 600
    onClosed: layerNum = 0

    // Close when the layer goes away (deleted here or elsewhere).
    Param { id: exists; path: root.base ? root.base + "/source" : "" }
    Connections {
        target: exists
        enabled: root.shown && root.layerNum > 0
        function onValueChanged() { if (!exists.exists) root.close() }
    }
    Param { id: chroma; path: root.base ? root.base + "/chroma/enabled" : "" }

    SettingSection { text: "中身"; topPadding: 0 }
    SettingRow {
        label: "ソース"
        ParamPick {
            objectName: "layerSource"
            path: root.base ? root.base + "/source" : ""
            title: "ソース"
            choices: function() { return Video.sourceChoices() }
        }
    }
    SettingRow {
        label: "名前"
        note: "空ならソースの名前"
        ParamText { path: root.base ? root.base + "/name" : ""; title: "レイヤーの名前"; emptyText: "（ソースの名前）" }
    }
    SettingRow {
        label: "収め方"
        note: "全体 = 縦横比を保って全部、埋める = はみ出しを切る、引き伸ばし = 枠に合わせる"
        ParamSegments {
            path: root.base ? root.base + "/fit" : ""
            labels: ({ fit: "全体", fill: "埋める", stretch: "引き伸ばし" })
        }
    }

    SettingSection { text: "位置と大きさ（キャンバス = 100 %）" }
    Flow {
        width: parent.width
        spacing: 6
        Repeater {
            model: root.presets
            delegate: MenuButton {
                required property var modelData
                width: (root.contentWidth - 6 * 4) / 5
                height: Theme.touch
                fontSize: 13
                label: modelData.label
                onTapped: root.applyBox(modelData.box)
            }
        }
    }
    Grid {
        width: parent.width
        columns: 4
        spacing: 6
        Repeater {
            model: [{ key: "x", label: "左" }, { key: "y", label: "上" }, { key: "width", label: "幅" }, { key: "height", label: "高さ" }]
            delegate: Column {
                required property var modelData
                width: (root.contentWidth - 6 * 3) / 4
                spacing: 2
                Text { color: Theme.subText; font.pixelSize: 12; text: modelData.label }
                ParamNumber {
                    width: parent.width
                    path: root.base ? root.base + "/" + modelData.key : ""
                    title: modelData.label + "（キャンバスの %）"
                    factor: 100
                    unit: "%"
                    decimals: 1
                }
            }
        }
    }
    SettingRow {
        label: "不透明度"
        controlWidth: 300
        ParamSlider { path: root.base ? root.base + "/opacity" : ""; from: 0; to: 1; defaultValue: 1 }
    }

    SettingSection { text: "切り取り（ソースの %）" }
    Grid {
        width: parent.width
        columns: 4
        spacing: 6
        Repeater {
            model: [{ key: "left", label: "左" }, { key: "top", label: "上" }, { key: "right", label: "右" }, { key: "bottom", label: "下" }]
            delegate: Column {
                required property var modelData
                width: (root.contentWidth - 6 * 3) / 4
                spacing: 2
                Text { color: Theme.subText; font.pixelSize: 12; text: modelData.label }
                ParamNumber {
                    width: parent.width
                    path: root.base ? root.base + "/crop/" + modelData.key : ""
                    title: "切り取り " + modelData.label + "（ソースの %）"
                    factor: 100
                    unit: "%"
                    decimals: 1
                }
            }
        }
    }

    SettingSection { text: "クロマキー" }
    SettingRow {
        label: "クロマキー"
        note: "グリーンバックなど、指定した色を透明にします"
        controlWidth: 100
        ParamSwitch { path: root.base ? root.base + "/chroma/enabled" : "" }
    }
    Column {
        width: parent.width
        visible: chroma.value === true
        SettingRow {
            label: "抜く色"
            controlWidth: 160
            ParamColor { path: root.base ? root.base + "/chroma/color" : ""; title: "抜く色"; alpha: false }
        }
        SettingRow {
            label: "色の範囲"
            controlWidth: 300
            ParamSlider { path: root.base ? root.base + "/chroma/similarity" : ""; from: 0; to: 1; defaultValue: 0.4 }
        }
        SettingRow {
            label: "境目のなめらかさ"
            controlWidth: 300
            ParamSlider { path: root.base ? root.base + "/chroma/smoothness" : ""; from: 0; to: 1; defaultValue: 0.08 }
        }
        SettingRow {
            label: "色かぶりの除去"
            controlWidth: 300
            ParamSlider { path: root.base ? root.base + "/chroma/spill" : ""; from: 0; to: 1; defaultValue: 0.1 }
        }
    }

    SettingSection { text: "その他" }
    SettingRow {
        label: "信号が途切れたとき"
        ParamSegments {
            path: root.base ? root.base + "/no_signal" : ""
            labels: ({ transparent: "透明", image: "「信号なし」" })
        }
    }
    Item { width: 1; height: 8 }
    SlideConfirm {
        objectName: "removeLayer"
        width: parent.width
        label: "スライドしてレイヤーを削除"
        onConfirmed: {
            const s = root.scene, l = root.layerNum
            root.close()
            params.call("/video/scene/" + s + "/layer/remove", { layer: l })
        }
    }
}
