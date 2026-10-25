import QtQuick
import DcUi.Params

// The video mixes (size, frame rate, background) and which mix goes to which output: recording,
// virtual camera, full-screen monitor, the remote's preview and LAN send.
Item {
    id: root
    signal mixPicked(int mix)

    ParamList { id: mixes; prefix: "/video/mix" }
    ParamList { id: displays; prefix: "/video/devices/display" }
    Param { id: vcamState; path: "/video/vcam/state" }
    Param { id: vcamDetail; path: "/video/vcam/detail" }
    Param { id: monitorState; path: "/video/monitor/state" }
    Param { id: monitorDetail; path: "/video/monitor/detail" }
    Param { id: monitorMix; path: "/video/monitor/mix" }
    Param { id: previewMix; path: "/video/preview/mix" }
    Param { id: lanMix; path: "/video/lan/mix" }

    function mixChoices() { return mixes.count, Video.mixChoices(true) }
    function displayChoices() {
        const list = [{ label: "主モニター", value: "" }]
        for (let i = 0; i < displays.count; ++i) {
            const b = "/video/devices/display/" + displays.numAt(i)
            list.push({ label: params.value(b + "/name") || "モニター " + displays.numAt(i), value: params.value(b + "/id"),
                        detail: Video.sizeText(params.value(b + "/width"), params.value(b + "/height"))
                                + (params.value(b + "/primary") ? "・主モニター" : "") })
        }
        return list
    }
    function fpsChoices(list) { return () => list.map(v => ({ label: v + " fps", value: v })) }

    Flickable {
        id: flick
        objectName: "outputsFlick"
        anchors.fill: parent
        contentHeight: column.height + 24
        boundsBehavior: Flickable.StopAtBounds
        clip: true
        Column {
            id: column
            x: 12
            width: Math.min(flick.width - 24, 860)

            SettingSection { text: "映像ミックス（最大 4）"; topPadding: 0 }
            Repeater {
                model: mixes
                delegate: Column {
                    id: mixItem
                    required property int num
                    required property string base
                    objectName: "mix_" + num
                    width: column.width
                    Param { id: mName; path: mixItem.base + "/name" }
                    Param { id: mActive; path: mixItem.base + "/active" }
                    Param { id: mProgram; path: mixItem.base + "/program_scene" }
                    Item {
                        width: parent.width
                        height: Theme.touch + 12
                        Text {
                            anchors { left: parent.left; right: mixButtons.left; rightMargin: 8; verticalCenter: parent.verticalCenter }
                            elide: Text.ElideRight
                            color: Theme.text
                            font.pixelSize: 17
                            font.bold: true
                            text: (mName.value || "ミックス " + mixItem.num)
                                  + (mActive.value ? "" : "（どこにも出していません）")
                        }
                        Row {
                            id: mixButtons
                            anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                            spacing: 6
                            MenuButton {
                                width: 132; height: Theme.touch; fontSize: 14
                                label: "シーンを編集"
                                onTapped: root.mixPicked(mixItem.num)
                            }
                            MenuButton {
                                objectName: "removeMix_" + mixItem.num
                                width: 44; height: Theme.touch
                                label: "✕"
                                active: mixes.count > 1
                                onTapped: {
                                    const n = mixItem.num
                                    Sheets.choice.open(Video.mixName(n) + " を削除しますか",
                                                       [{ label: "削除する", detail: "このミックスを出していた出力先は「なし」になります", value: true },
                                                        { label: "やめる", value: false }],
                                                       c => { if (c.value) params.call("/video/mix/remove", { mix: n }) })
                                }
                            }
                        }
                    }
                    SettingRow {
                        label: "名前"
                        ParamText { path: mixItem.base + "/name"; title: "ミックスの名前"; emptyText: "ミックス " + mixItem.num }
                    }
                    SettingRow {
                        label: "解像度"
                        SizePick { prefix: mixItem.base; title: "合成の解像度" }
                    }
                    SettingRow {
                        label: "フレームレート"
                        ParamPick { path: mixItem.base + "/fps"; title: "合成のフレームレート"; choices: root.fpsChoices([24, 30, 60, 120]) }
                    }
                    SettingRow {
                        label: "背景色"
                        note: "レイヤーのない所の色"
                        controlWidth: 160
                        ParamColor { path: mixItem.base + "/background"; title: "背景色" }
                    }
                    InfoRow {
                        label: "本番のシーン"
                        value: (mProgram.value, Video.sceneName(mProgram.value))
                    }
                    Item { width: 1; height: 12 }
                }
            }
            MenuButton {
                objectName: "addMix"
                width: parent.width
                height: Theme.touch
                label: "+ 映像ミックスを追加"
                active: mixes.count < 4
                onTapped: params.call("/video/mix/add")
            }

            SettingSection { text: "録画" }
            SettingRow {
                label: "録画するミックス"
                ParamPick { objectName: "captureMix"; path: "/capture/mix"; title: "録画するミックス"; choices: root.mixChoices }
            }

            SettingSection { text: "仮想カメラ" }
            SettingRow {
                label: "出すミックス"
                note: "Zoom や Discord などでカメラとして選べます"
                ParamPick { objectName: "vcamMix"; path: "/video/vcam/mix"; title: "仮想カメラに出すミックス"; choices: root.mixChoices }
            }
            SettingRow {
                label: "解像度"
                ParamSegments { path: "/video/vcam/resolution" }
            }
            InfoRow {
                label: "状態"
                value: vcamState.value === "active" ? "使われています" : vcamState.value === "waiting" ? "待機中（使っているアプリなし）"
                     : vcamState.value === "unavailable" ? "使えません" : "オフ"
                valueColor: vcamState.value === "active" ? Theme.ok : vcamState.value === "unavailable" ? Theme.mute : Theme.subText
                note: vcamDetail.value
            }

            SettingSection { text: "全画面表示" }
            SettingRow {
                label: "出すミックス"
                note: "別のモニターに映像を全画面で出します（プロジェクター・配信用モニターなど）"
                ParamPick { objectName: "monitorMix"; path: "/video/monitor/mix"; title: "全画面表示に出すミックス"; choices: root.mixChoices }
            }
            SettingRow {
                visible: monitorMix.value > 0
                label: "表示するモニター"
                ParamPick { path: "/video/monitor/display"; title: "表示するモニター"; emptyText: "主モニター"; choices: root.displayChoices }
            }
            InfoRow {
                label: "状態"
                value: monitorState.value === "active" ? "表示中" : monitorState.value === "no_display" ? "モニターが見つかりません"
                     : monitorState.value === "error" ? "エラー" : "オフ"
                valueColor: monitorState.value === "active" ? Theme.ok : monitorState.value === "off" ? Theme.subText : Theme.mute
                note: monitorDetail.value
            }

            SettingSection { text: "リモコンのプレビュー" }
            SettingRow {
                label: "出すミックス"
                ParamPick { path: "/video/preview/mix"; title: "プレビューに出すミックス"; choices: root.mixChoices }
            }
            Column {
                width: parent.width
                visible: previewMix.value > 0
                SettingRow {
                    label: "解像度"
                    SizePick { prefix: "/video/preview"; title: "プレビューの解像度"
                               presets: [{ w: 1920, h: 1080 }, { w: 1280, h: 720 }, { w: 960, h: 540 }, { w: 640, h: 360 }] }
                }
                SettingRow {
                    label: "フレームレート"
                    ParamPick { path: "/video/preview/fps"; title: "プレビューのフレームレート"; choices: root.fpsChoices([10, 15, 30, 60]) }
                }
                SettingRow {
                    label: "ビットレート"
                    controlWidth: 160
                    ParamNumber { path: "/video/preview/bitrate_kbps"; title: "プレビューのビットレート"; unit: "kbps" }
                }
            }

            SettingSection { text: "LAN 送信（別の PC へ）" }
            SettingRow {
                label: "出すミックス"
                ParamPick { path: "/video/lan/mix"; title: "LAN 送信に出すミックス"; choices: root.mixChoices }
            }
            Column {
                width: parent.width
                visible: lanMix.value > 0
                SettingRow {
                    label: "解像度"
                    SizePick { prefix: "/video/lan"; title: "LAN 送信の解像度" }
                }
                SettingRow {
                    label: "フレームレート"
                    ParamPick { path: "/video/lan/fps"; title: "LAN 送信のフレームレート"; choices: root.fpsChoices([15, 30, 60]) }
                }
                SettingRow {
                    label: "ビットレート"
                    controlWidth: 160
                    ParamNumber { path: "/video/lan/bitrate_kbps"; title: "LAN 送信のビットレート"; unit: "kbps" }
                }
            }
        }
    }
}
