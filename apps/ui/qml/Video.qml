pragma Singleton
import QtQuick

// Names and choice lists shared by the video screens (protocol.md 4.5), read from the tree.
QtObject {
    readonly property var kinds: [
        { value: "display", label: "モニター", detail: "画面全体を取り込む" },
        { value: "window", label: "ウィンドウ", detail: "1 つのアプリの画面" },
        { value: "camera", label: "カメラ", detail: "Web カメラ・キャプチャーボード" },
        { value: "image", label: "画像", detail: "PNG・JPEG など" },
        { value: "media", label: "動画", detail: "MP4 など（繰り返し再生できます）" },
        { value: "text", label: "文字", detail: "テロップ・タイトル" },
        { value: "color", label: "単色", detail: "背景や帯" },
        { value: "lan", label: "LAN", detail: "別の PC から届く映像" },
    ]

    function kindLabel(kind) {
        for (let i = 0; i < kinds.length; ++i)
            if (kinds[i].value === kind) return kinds[i].label
        return kind || ""
    }
    function sourceName(n) {
        if (!n) return "なし"
        const b = "/video/source/" + n
        if (!params.exists(b + "/kind")) return "（消えたソース " + n + "）"
        return params.value(b + "/name") || kindLabel(params.value(b + "/kind")) + " " + n
    }
    function sceneName(n) {
        if (!n) return "なし"
        return params.value("/video/scene/" + n + "/name") || "シーン " + n
    }
    function mixName(n) {
        if (!n) return "なし"
        return params.value("/video/mix/" + n + "/name") || "ミックス " + n
    }
    function layerName(scene, layer) {
        const b = "/video/scene/" + scene + "/layer/" + layer
        return params.value(b + "/name") || sourceName(params.value(b + "/source"))
    }
    function stateText(s) {
        return s === "ok" ? "取り込み中" : s === "disconnected" ? "切断中" : s === "error" ? "エラー" : "休止"
    }
    function stateColor(s) {
        return s === "ok" ? Theme.ok : s === "disconnected" ? Theme.warn : s === "error" ? Theme.mute : Theme.subText
    }
    // [{label, detail, value}] of the mixes, "なし" (0) first when withNone.
    function mixChoices(withNone) {
        const list = withNone ? [{ label: "なし", value: 0 }] : []
        const nums = params.indices("/video/mix")
        for (let i = 0; i < nums.length; ++i) {
            const b = "/video/mix/" + nums[i]
            list.push({ label: mixName(nums[i]), value: nums[i],
                        detail: params.value(b + "/width") + "×" + params.value(b + "/height") + " " + params.value(b + "/fps") + "fps" })
        }
        return list
    }
    function sourceChoices() {
        const list = []
        const nums = params.indices("/video/source")
        for (let i = 0; i < nums.length; ++i) {
            const b = "/video/source/" + nums[i]
            list.push({ label: sourceName(nums[i]), value: nums[i], detail: kindLabel(params.value(b + "/kind")) })
        }
        return list
    }
    function sizeText(w, h) { return w > 0 && h > 0 ? w + "×" + h : "" }
    // 0xAARRGGBB -> "#aarrggbb" for QML colours.
    function argb(v) { return "#" + ("0000000" + Math.round(Number(v) || 0).toString(16)).slice(-8) }
}
