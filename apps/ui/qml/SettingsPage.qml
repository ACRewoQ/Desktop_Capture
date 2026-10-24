import QtQuick
import DcUi.Params

// Engine settings (/system) and its state.
Item {
    id: page
    readonly property bool swipeable: true

    Param { id: rate; path: "/system/sample_rate" }
    Param { id: blockSize; path: "/system/block_size" }
    Param { id: clockActive; path: "/system/clock_active" }
    Param { id: latency; path: "/system/latency_ms" }
    Param { id: cpu; path: "/system/cpu_load" }
    Param { id: xruns; path: "/system/xruns" }
    Param { id: version; path: "/system/version" }
    ParamList { id: outputs; prefix: "/devices/output" }

    function outputName(id) {
        for (let i = 0; i < outputs.count; ++i) {
            const b = "/devices/output/" + outputs.numAt(i)
            if (params.value(b + "/id") === id) return params.value(b + "/name")
        }
        return id
    }
    function clockText(v) {
        if (!v) return "—"
        if (v === "timer") return "内部タイマー（出力デバイスなし）"
        if (v.startsWith("device/")) return outputName(v.substring(7))
        return v
    }

    Flickable {
        id: flick
        objectName: "settingsFlick"
        anchors.fill: parent
        contentHeight: column.height + 32
        boundsBehavior: Flickable.StopAtBounds
        clip: true

        Column {
            id: column
            x: 24
            width: Math.min(flick.width - 48, 860)

            SettingSection { text: "音声エンジン" }
            SettingRow {
                label: "バッファ"
                note: "小さいほど遅延が減り、音切れしやすくなります（" +
                      (blockSize.value / rate.value * 1000).toFixed(1) + " ms）"
                controlWidth: 360
                ParamSegments { path: "/system/block_size" }
            }
            SettingRow {
                label: "動作モード"
                note: "排他にするとデバイスを独占し、遅延が小さくなります"
                ParamSegments {
                    path: "/system/audio_mode"
                    labels: ({ shared: "共有", exclusive: "排他" })
                }
            }
            SettingRow {
                label: "クロック基準"
                note: "音声処理のタイミングを合わせる出力デバイス"
                ParamPick {
                    objectName: "clockDevice"
                    path: "/system/clock_device"
                    title: "クロック基準"
                    emptyText: "自動"
                    choices: function() {
                        const list = [{ label: "自動", value: "" }]
                        for (let i = 0; i < outputs.count; ++i) {
                            const b = "/devices/output/" + outputs.numAt(i)
                            list.push({ label: params.value(b + "/name"), value: params.value(b + "/id"),
                                        detail: params.value(b + "/connected") ? "" : "切断中" })
                        }
                        return list
                    }
                }
            }

            SettingSection { text: "デバイス" }
            Repeater {
                model: [{ prefix: "/devices/input", label: "入力" }, { prefix: "/devices/output", label: "出力" }]
                delegate: Column {
                    id: group
                    required property var modelData
                    width: column.width
                    ParamList { id: devices; prefix: group.modelData.prefix }
                    Repeater {
                        model: devices
                        delegate: InfoRow {
                            id: dev
                            required property string base
                            Param { id: devName; path: dev.base + "/name" }
                            Param { id: devApi; path: dev.base + "/api" }
                            Param { id: devRate; path: dev.base + "/sample_rate" }
                            Param { id: devChannels; path: dev.base + "/channels" }
                            Param { id: devConnected; path: dev.base + "/connected" }
                            label: group.modelData.label + ": " + devName.value
                            note: [devApi.value === "asio" ? "ASIO" : devApi.value === "vmic" ? "仮想ケーブル" : "WASAPI",
                                   devChannels.value + " ch",
                                   devRate.value > 0 ? (devRate.value / 1000) + " kHz" : ""].filter(x => x).join(" · ")
                            value: devConnected.value ? "接続中" : "切断中"
                            valueColor: devConnected.value ? Theme.ok : Theme.mute
                        }
                    }
                }
            }
            Item { width: 1; height: 8 }
            MenuButton {
                objectName: "refreshDevices"
                width: 220
                height: Theme.touch
                label: "一覧を更新"
                onTapped: params.call("/devices/refresh")
            }

            SettingSection { text: "状態" }
            InfoRow {
                label: "使用中のクロック"
                value: page.clockText(clockActive.value)
            }
            InfoRow {
                label: "出力の遅延"
                value: Number(latency.value).toFixed(1) + " ms"
            }
            InfoRow {
                label: "音声処理の負荷"
                value: Math.round(cpu.value * 100) + " %"
            }
            InfoRow {
                label: "音切れ"
                note: "エンジンの起動後の回数"
                objectName: "xruns"
                value: xruns.value + " 回"
                valueColor: xruns.value > 0 ? Theme.warn : Theme.text
            }
            InfoRow {
                label: "サンプルレート"
                value: (rate.value / 1000) + " kHz"
            }

            SettingSection { text: "接続" }
            InfoRow {
                label: "エンジン"
                value: (connection.serverName || "—") + "  " + (version.value || "")
            }
            InfoRow {
                label: "アドレス"
                value: connection.address
            }
        }
    }
}
