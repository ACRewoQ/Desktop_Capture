import QtQuick
import DcUi.Params

// Settings of one source; the fields depend on its kind (fixed when it was added). Monitors, windows
// and cameras are picked from the lists capd reports (/video/devices/*).
Sheet {
    id: root
    property int source: 0
    readonly property string base: source > 0 ? "/video/source/" + source : ""

    function open(n) {
        source = n
        title = Video.sourceName(n)
        shown = true
        if (kind.value === "window" || kind.value === "camera") params.call("/video/devices/refresh")
    }
    function deviceList(kindName) {
        const list = []
        const nums = params.indices("/video/devices/" + kindName)
        for (const n of nums) {
            const b = "/video/devices/" + kindName + "/" + n
            if (kindName === "display")
                list.push({ label: params.value(b + "/name") || "モニター " + n, value: params.value(b + "/id"),
                            detail: Video.sizeText(params.value(b + "/width"), params.value(b + "/height"))
                                    + (params.value(b + "/primary") ? "・主モニター" : "") })
            else if (kindName === "window")
                list.push({ label: params.value(b + "/title") || params.value(b + "/exe"), detail: params.value(b + "/exe"),
                            exe: params.value(b + "/exe"), windowTitle: params.value(b + "/title") })
            else
                list.push({ label: params.value(b + "/name") || params.value(b + "/id"), value: params.value(b + "/id"),
                            name: params.value(b + "/name") })
        }
        return list
    }
    function pickWindow() {
        params.call("/video/devices/refresh")
        windowListDelay.restart() // capd answers the refresh with a new list shortly after
    }
    function pickCamera() {
        const list = deviceList("camera")
        if (list.length === 0) list.push({ label: "カメラが見つかりません", disabled: true })
        const b = base
        Sheets.choice.open("カメラ", list.map(c => Object.assign({ selected: c.value === cameraId.value }, c)), c => {
            const ch = {}
            ch[b + "/camera/id"] = c.value
            ch[b + "/camera/name"] = c.name
            params.setMany(ch)
        })
    }
    function pickSize(prefix, presets, title) {
        const b = base
        const w = params.value(b + prefix + "/width"), h = params.value(b + prefix + "/height")
        Sheets.choice.open(title, presets.map(p => ({ label: p.label, value: p, selected: p.w === w && p.h === h })), c => {
            const ch = {}
            ch[b + prefix + "/width"] = c.value.w
            ch[b + prefix + "/height"] = c.value.h
            params.setMany(ch)
        })
    }
    function lanChoices() {
        const list = []
        for (const n of params.indices("/lan/peers")) {
            const b = "/lan/peers/" + n
            const id = params.value(b + "/id")
            const name = params.value(b + "/name") || id
            const st = params.value(b + "/state")
            list.push({ header: name + (st === "connected" ? "" : "（" + (st === "offline" ? "見つかりません" : st === "unpaired" ? "ペアリング前" : st) + "）") })
            list.push({ label: "映像ミックスの LAN 送信", detail: name, value: id + "/video/lan" })
            list.push({ label: "リモコン用プレビュー", detail: name, value: id + "/video/preview" })
        }
        if (list.length === 0) list.push({ label: "LAN に別の PC がありません", detail: "LAN ページでペアリングできます", disabled: true })
        return list
    }

    panelWidth: 600
    onClosed: source = 0

    Param { id: kind; path: root.base ? root.base + "/kind" : "" }
    Param { id: srcState; path: root.base ? root.base + "/state" : "" }
    Param { id: srcError; path: root.base ? root.base + "/error" : "" }
    Param { id: width_; path: root.base ? root.base + "/width" : "" }
    Param { id: height_; path: root.base ? root.base + "/height" : "" }
    Param { id: displayId; path: root.base ? root.base + "/display/id" : "" }
    Param { id: windowExe; path: root.base ? root.base + "/window/exe" : "" }
    Param { id: windowTitle; path: root.base ? root.base + "/window/title" : "" }
    Param { id: cameraId; path: root.base ? root.base + "/camera/id" : "" }
    Param { id: cameraName; path: root.base ? root.base + "/camera/name" : "" }
    Param { id: cameraWidth; path: root.base ? root.base + "/camera/width" : "" }
    Param { id: cameraHeight; path: root.base ? root.base + "/camera/height" : "" }
    Param { id: cameraFps; path: root.base ? root.base + "/camera/fps" : "" }
    Connections {
        target: kind
        enabled: root.shown && root.source > 0
        function onValueChanged() { if (!kind.exists) root.close() }
    }
    Timer {
        id: windowListDelay
        interval: 400
        onTriggered: {
            if (!root.shown) return
            const list = root.deviceList("window")
            if (list.length === 0) list.push({ label: "ウィンドウが見つかりません", detail: "録画プロセス（capd）が動いているか確認してください", disabled: true })
            const b = root.base
            Sheets.choice.open("ウィンドウ", list.map(c => Object.assign({ selected: c.exe === windowExe.value && c.windowTitle === windowTitle.value }, c)), c => {
                const ch = {}
                ch[b + "/window/exe"] = c.exe
                ch[b + "/window/title"] = c.windowTitle
                params.setMany(ch)
            })
        }
    }

    SettingSection { text: Video.kindLabel(kind.value); topPadding: 0 }
    InfoRow {
        label: "状態"
        value: Video.stateText(srcState.value) + (Video.sizeText(width_.value, height_.value) ? "  " + Video.sizeText(width_.value, height_.value) : "")
        valueColor: Video.stateColor(srcState.value)
        note: srcState.value === "error" ? srcError.value : srcState.value === "none" ? "どのミックスの本番・次のシーンにも出ていません" : ""
    }
    SettingRow {
        label: "名前"
        ParamText { path: root.base ? root.base + "/name" : ""; title: "ソースの名前"; emptyText: "（" + Video.kindLabel(kind.value) + " " + root.source + "）" }
    }

    // display
    Column {
        width: parent.width
        visible: kind.value === "display"
        SettingRow {
            label: "モニター"
            ParamPick {
                objectName: "displayPick"
                path: root.base ? root.base + "/display/id" : ""
                title: "モニター"
                emptyText: "主モニター"
                choices: function() { return [{ label: "主モニター", value: ""}].concat(root.deviceList("display")) }
            }
        }
        SettingRow {
            label: "取得方式"
            note: "うまく映らないときは DDA を試してください"
            ParamSegments { path: root.base ? root.base + "/display/method" : ""; labels: ({ wgc: "WGC（標準）", dda: "DDA" }) }
        }
    }

    // window
    Column {
        width: parent.width
        visible: kind.value === "window"
        SettingRow {
            label: "ウィンドウ"
            note: windowExe.value ? windowExe.value : ""
            MenuButton {
                objectName: "windowPick"
                width: parent.width
                height: Theme.touch
                fontSize: 15
                label: windowTitle.value || windowExe.value || "選ぶ"
                onTapped: root.pickWindow()
            }
        }
        SettingRow {
            label: "閉じたあと"
            note: "同じアプリが開き直したときにつなぎ直す条件"
            ParamSegments { path: root.base ? root.base + "/window/match" : ""; labels: ({ title: "名前とタイトル", exe: "アプリ名だけ" }) }
        }
    }
    SettingRow {
        visible: kind.value === "display" || kind.value === "window"
        label: "マウスカーソルを映す"
        controlWidth: 100
        ParamSwitch { path: root.base ? root.base + "/cursor" : "" }
    }

    // camera
    Column {
        width: parent.width
        visible: kind.value === "camera"
        SettingRow {
            label: "カメラ"
            MenuButton {
                objectName: "cameraPick"
                width: parent.width
                height: Theme.touch
                fontSize: 15
                label: cameraName.value || cameraId.value || "選ぶ"
                onTapped: root.pickCamera()
            }
        }
        SettingRow {
            label: "解像度"
            MenuButton {
                width: parent.width
                height: Theme.touch
                fontSize: 15
                label: cameraWidth.value > 0 ? cameraWidth.value + "×" + cameraHeight.value : "自動（1080p まで）"
                onTapped: root.pickSize("/camera", [{ label: "自動（1080p まで）", w: 0, h: 0 }, { label: "3840×2160", w: 3840, h: 2160 },
                                                    { label: "1920×1080", w: 1920, h: 1080 }, { label: "1280×720", w: 1280, h: 720 },
                                                    { label: "640×480", w: 640, h: 480 }], "カメラの解像度")
            }
        }
        SettingRow {
            label: "フレームレート"
            ParamPick {
                path: root.base ? root.base + "/camera/fps" : ""
                title: "カメラのフレームレート"
                choices: function() { return [{ label: "自動（60 まで）", value: 0 }, { label: "60", value: 60 }, { label: "30", value: 30 }, { label: "24", value: 24 }, { label: "15", value: 15 }] }
            }
        }
    }

    // image / media
    Column {
        width: parent.width
        visible: kind.value === "image" || kind.value === "media"
        SettingRow {
            label: "ファイル"
            note: "エンジンの PC のパス（例 C:\\Users\\…\\logo.png）"
            ParamText { path: root.base ? root.base + "/file" : ""; title: "ファイルのパス"; emptyText: "未設定" }
        }
        SettingRow {
            visible: kind.value === "media"
            label: "繰り返し再生"
            controlWidth: 100
            ParamSwitch { path: root.base ? root.base + "/media/loop" : "" }
        }
    }

    // text
    Column {
        width: parent.width
        visible: kind.value === "text"
        SettingRow {
            label: "文字"
            controlWidth: 340
            ParamText { objectName: "textContent"; path: root.base ? root.base + "/text/content" : ""; title: "表示する文字"; multiline: true; emptyText: "未入力" }
        }
        SettingRow {
            label: "フォント"
            ParamText { path: root.base ? root.base + "/text/font" : ""; title: "フォント名" }
        }
        SettingRow {
            label: "大きさ"
            controlWidth: 160
            ParamNumber { path: root.base ? root.base + "/text/size" : ""; title: "文字の大きさ"; unit: "px" }
        }
        SettingRow {
            label: "文字色"
            controlWidth: 160
            ParamColor { path: root.base ? root.base + "/text/color" : ""; title: "文字色" }
        }
        SettingRow {
            label: "背景"
            note: "透明にするには不透明度を 0 に"
            controlWidth: 160
            ParamColor { path: root.base ? root.base + "/text/background" : ""; title: "文字の背景" }
        }
        SettingRow {
            label: "太字"
            controlWidth: 100
            ParamSwitch { path: root.base ? root.base + "/text/bold" : "" }
        }
        SettingRow {
            label: "斜体"
            controlWidth: 100
            ParamSwitch { path: root.base ? root.base + "/text/italic" : "" }
        }
        SettingRow {
            label: "行揃え"
            ParamSegments { path: root.base ? root.base + "/text/align" : ""; labels: ({ left: "左", center: "中央", right: "右" }) }
        }
        SettingRow {
            label: "折り返し幅"
            note: "0 は折り返さない"
            controlWidth: 160
            ParamNumber { path: root.base ? root.base + "/text/wrap_width" : ""; title: "折り返し幅"; unit: "px" }
        }
    }

    // color
    SettingRow {
        visible: kind.value === "color"
        label: "色"
        controlWidth: 160
        ParamColor { path: root.base ? root.base + "/color/value" : ""; title: "単色ソースの色" }
    }

    // lan
    SettingRow {
        visible: kind.value === "lan"
        label: "受け取る映像"
        ParamPick {
            path: root.base ? root.base + "/lan/stream" : ""
            title: "受け取る映像"
            emptyText: "未設定"
            choices: function() { return root.lanChoices() }
        }
    }

    Item { width: 1; height: 8 }
    SlideConfirm {
        objectName: "removeSource"
        width: parent.width
        label: "スライドしてソースを削除（レイヤーも消えます）"
        onConfirmed: {
            const n = root.source
            root.close()
            params.call("/video/source/remove", { source: n })
        }
    }
}
