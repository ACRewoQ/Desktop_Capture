pragma Singleton
import QtQuick

// Words and choice lists for the hotkey and MIDI bindings (protocol.md 5.9), read from the tree.
// The screens show "modes": the engine's actions, with scene and bank switching split out of "set".
QtObject {
    readonly property var modes: [
        { value: "record_toggle", label: "録画の開始・停止", detail: "押すたびに始める・止める" },
        { value: "record_start", label: "録画を始める" },
        { value: "record_stop", label: "録画を止める" },
        { value: "recorder_toggle", label: "トラック別録音の開始・停止" },
        { value: "pad", label: "サンプラーのパッド", detail: "「押す間」のパッドは離すと止まる" },
        { value: "pad_stop", label: "サンプラーを止める" },
        { value: "bank", label: "サンプラーのバンク切替" },
        { value: "scene", label: "シーン切替", detail: "本番のシーンを替える" },
        { value: "take", label: "TAKE", detail: "次のシーンを本番に出す" },
        { value: "snapshot", label: "スナップショット呼び出し", detail: "ミキサーの値を撮ったときに戻す" },
        { value: "toggle", label: "オン・オフの切り替え", detail: "ミュート・ソロ・キュー・録音・エフェクトのバイパス" },
        { value: "step", label: "値の上げ下げ", detail: "音量など。キーを押し続けると続けて変わる" },
        { value: "param", label: "MIDI のフェーダー・つまみで動かす", detail: "音量・パン・フィルター" },
        { value: "set", label: "値を決める（パスで指定）", detail: "ほかのパラメーター" },
    ]

    function modeOf(action, target) {
        if (action === "set") {
            if (/^\/video\/mix\/\d+\/program_scene$/.test(target || "")) return "scene"
            if (target === "/sampler/active_bank") return "bank"
        }
        return action || "none"
    }
    function modeLabel(mode) {
        for (let i = 0; i < modes.length; ++i)
            if (modes[i].value === mode) return modes[i].label
        return "なし"
    }
    // The action, target and value a mode starts with.
    function modeDefaults(mode) {
        switch (mode) {
        case "pad": return { action: "pad", target: "1", value: "" }
        case "pad_stop": return { action: "pad_stop", target: "", value: "" }
        case "bank": return { action: "set", target: "/sampler/active_bank", value: "1" }
        case "scene": return { action: "set", target: "/video/mix/1/program_scene", value: String(firstScene()) }
        case "take": return { action: "take", target: "1", value: "" }
        case "snapshot": return { action: "snapshot", target: String(firstSnapshot()), value: "" }
        case "toggle": return { action: "toggle", target: firstStrip() + "/mute", value: "" }
        case "step": return { action: "step", target: "/master/volume_db", value: "-1" }
        case "param": return { action: "param", target: "/master/volume_db", value: "" }
        case "set": return { action: "set", target: "", value: "" }
        default: return { action: mode, target: "", value: "" }
        }
    }

    // --- triggers ---
    readonly property var keyWords: ({
        space: "Space", enter: "Enter", tab: "Tab", backspace: "BackSpace", escape: "Esc", insert: "Insert",
        delete: "Delete", home: "Home", end: "End", pageup: "PageUp", pagedown: "PageDown", up: "↑", down: "↓",
        left: "←", right: "→", pause: "Pause", printscreen: "PrintScreen", scrolllock: "ScrollLock", minus: "-",
        equal: "=", comma: ",", period: ".", semicolon: ";", slash: "/", backslash: "\\", quote: "'",
        bracketleft: "[", bracketright: "]", backquote: "`", num_add: "テンキー +", num_sub: "テンキー -",
        num_mul: "テンキー *", num_div: "テンキー /", num_dec: "テンキー .", media_play: "再生/一時停止",
        media_stop: "停止（メディア）", media_next: "次の曲", media_prev: "前の曲"
    })
    function keyWord(k) {
        if (keyWords[k] !== undefined) return keyWords[k]
        if (/^num\d$/.test(k)) return "テンキー " + k.substring(3)
        return k.toUpperCase()
    }
    function triggerText(trigger) {
        const t = String(trigger || "")
        if (t === "") return "未設定"
        if (t.startsWith("key:")) {
            const mods = { ctrl: "Ctrl", alt: "Alt", shift: "Shift", win: "Win" }
            return t.substring(4).split("+").map(p => mods[p] || keyWord(p)).join(" + ")
        }
        if (t.startsWith("midi:")) {
            const p = t.substring(5).split("/")
            const ch = "（ch " + p[1] + "）"
            if (p[0] === "note") return "MIDI ノート " + p[2] + ch
            if (p[0] === "cc") return "MIDI CC " + p[2] + ch
            if (p[0] === "pc") return "MIDI プログラム " + p[2] + ch
            if (p[0] === "pb") return "MIDI ピッチベンド" + ch
        }
        return t
    }
    // What someone types ("Ctrl+Shift+F9", "midi:cc/1/7") as a trigger string.
    function normalizeTrigger(text) {
        const t = String(text || "").trim().replace(/\s+/g, "").toLowerCase()
        if (t === "" || t.startsWith("key:") || t.startsWith("midi:")) return t
        return "key:" + t
    }

    // --- targets ---
    function stripName(base) {
        if (base === "/master") return "マスター"
        return params.value(base + "/name") || ("トラック " + base.split("/")[2])
    }
    readonly property var paramWords: ({
        volume_db: "音量", pan: "パン", mute: "ミュート", solo: "ソロ", cue: "キュー", "record/enabled": "録音",
        "lowcut/enabled": "ローカット", "lowcut/freq_hz": "ローカット周波数", "highcut/enabled": "ハイカット",
        "highcut/freq_hz": "ハイカット周波数"
    })
    function targetText(path) {
        const p = String(path || "")
        if (p === "") return "未設定"
        if (p === "/sampler/active_bank") return "サンプラーのバンク"
        if (p === "/cue/volume_db") return "キューの音量"
        const m = /^(\/track\/\d+|\/master)\/(.+)$/.exec(p)
        if (!m) return p
        const fx = /^fx\/(\d+)\/(bypass|param\/(.+))$/.exec(m[2])
        if (fx) {
            const name = params.value(m[1] + "/fx/" + fx[1] + "/name") || ("エフェクト " + fx[1])
            return stripName(m[1]) + " の " + name + (fx[2] === "bypass" ? " のバイパス" : " の " + fx[3])
        }
        return stripName(m[1]) + " の " + (paramWords[m[2]] || m[2])
    }
    function actionText(action, target, value) {
        const mode = modeOf(action, target)
        switch (mode) {
        case "none": return "なし"
        case "pad": {
            const p = String(target || "").split("/")
            return p.length === 2 ? "パッド " + p[1] + "（バンク " + p[0] + "）" : "パッド " + p[0] + "（今のバンク）"
        }
        case "pad_stop": return target ? "サンプラーを止める（バンク " + target + "）" : "サンプラーを全部止める"
        case "bank": return "バンク " + value + " に切り替え"
        case "scene": {
            const mix = target.split("/")[3]
            return Video.mixName(Number(mix)) + " を " + Video.sceneName(Number(value)) + " に"
        }
        case "take": return "TAKE（" + Video.mixName(Number(target)) + "）"
        case "snapshot": return "スナップショット「" + snapshotName(Number(target)) + "」"
        case "toggle": return targetText(target) + " のオン・オフ"
        case "step": return targetText(target) + " を " + (Number(value) > 0 ? "+" : "") + value
        case "param": return targetText(target) + " を動かす"
        case "set": return targetText(target) + " = " + value
        default: return modeLabel(mode)
        }
    }
    function stateText(s) {
        return s === "ok" ? "使えます" : s === "off" ? "オフ" : s === "invalid" ? "設定が正しくありません"
             : s === "conflict" ? "ほかのアプリが使用中" : s === "unsupported" ? "この PC では使えません" : ""
    }
    function stateColor(s) {
        return s === "ok" ? Theme.ok : s === "off" ? Theme.subText : s === "conflict" ? Theme.warn : Theme.mute
    }
    function midiStateText(s) {
        return s === "ok" ? "受信中" : s === "busy" ? "ほかのアプリが使用中" : s === "error" ? "開けません" : "使わない"
    }

    // --- choice lists ---
    function tracksInOrder() {
        const nums = params.indices("/track")
        return nums.slice().sort((a, b) => Number(params.value("/track/" + a + "/position")) - Number(params.value("/track/" + b + "/position")))
    }
    function firstStrip() {
        const t = tracksInOrder()
        return t.length > 0 ? "/track/" + t[0] : "/master"
    }
    function firstScene() {
        const s = params.indices("/video/scene")
        return s.length > 0 ? s[0] : 1
    }
    function stripChoices(current) {
        const list = tracksInOrder().map(n => ({ label: stripName("/track/" + n), value: "/track/" + n }))
        list.push({ label: "マスター", value: "/master" })
        list.forEach(c => c.selected = String(current || "").startsWith(c.value + "/"))
        return list
    }
    // The parameters of a strip a mode can drive.
    function paramChoices(base, mode) {
        const list = []
        const add = (key, detail) => list.push({ label: paramWords[key], value: base + "/" + key, detail: detail || "" })
        if (mode === "toggle") {
            add("mute")
            if (base !== "/master") { add("solo"); add("cue") }
            add("record/enabled", "トラック別録音の対象")
            add("lowcut/enabled")
            add("highcut/enabled")
            const fx = params.indices(base + "/fx")
            for (let i = 0; i < fx.length; ++i)
                list.push({ label: (params.value(base + "/fx/" + fx[i] + "/name") || "エフェクト " + fx[i]) + " のバイパス",
                            value: base + "/fx/" + fx[i] + "/bypass" })
        } else {
            add("volume_db", "フェーダーと同じ目盛り")
            add("pan")
            add("lowcut/freq_hz")
            add("highcut/freq_hz")
        }
        return list
    }
    function bankChoices(withActive, current) {
        const list = withActive ? [{ label: "今のバンク", value: 0, selected: current === 0 }] : []
        for (let b = 1; b <= 8; ++b)
            list.push({ label: params.value("/sampler/bank/" + b + "/name") || "バンク " + b, value: b, selected: current === b })
        return list
    }
    function padChoices(bank, current) {
        const list = []
        for (let p = 1; p <= 16; ++p) {
            const b = "/sampler/bank/" + (bank || params.value("/sampler/active_bank") || 1) + "/pad/" + p
            const file = String(params.value(b + "/file") || "")
            list.push({ label: "パッド " + p + (params.value(b + "/name") ? "：" + params.value(b + "/name") : ""),
                        detail: file ? file.split(/[\\/]/).pop() : "空", value: p, selected: current === p })
        }
        return list
    }
    function snapshotName(n) {
        const name = params.value("/snapshot/" + n + "/name")
        return name !== undefined ? (name || ("Snapshot " + n)) : n + "（ない）"
    }
    function firstSnapshot() {
        const nums = params.indices("/snapshot")
        return nums.length > 0 ? nums[0] : 1
    }
    function snapshotChoices(current) {
        return params.indices("/snapshot").map(n => ({ label: snapshotName(n), value: n, selected: current === n }))
    }
    function sceneChoices(current) {
        const nums = params.indices("/video/scene")
        return nums.map(n => ({ label: Video.sceneName(n), value: n, selected: current === n }))
    }
    function midiDeviceChoices(current) {
        const list = [{ label: "どの機器でも", value: "", selected: !current }]
        const nums = params.indices("/control/midi/device")
        for (let i = 0; i < nums.length; ++i) {
            const name = params.value("/control/midi/device/" + nums[i] + "/name")
            list.push({ label: name, value: name, selected: current === name })
        }
        if (current && !list.some(c => c.value === current)) list.push({ label: current + "（今はつながっていない）", value: current, selected: true })
        return list
    }
}
