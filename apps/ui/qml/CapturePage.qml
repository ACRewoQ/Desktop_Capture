import QtQuick
import DcUi.Params

// Recording settings: the video file (/capture, started from the top bar) and the per-track audio
// recording (/recorder, WAV / FLAC per track).
Item {
    id: page
    readonly property bool swipeable: true

    Param { id: captureState; path: "/capture/state" }
    Param { id: rateControl; path: "/capture/video/rate_control" }
    Param { id: container; path: "/capture/container" }
    Param { id: recState; path: "/recorder/state" }
    Param { id: recElapsed; path: "/recorder/elapsed_ms" }
    Param { id: recFolder; path: "/recorder/folder" }
    Param { id: recError; path: "/recorder/error" }
    Param { id: recDropped; path: "/recorder/dropped_frames" }
    Param { id: masterArmed; path: "/master/record/enabled" }
    ParamList { id: trackList; prefix: "/track" }
    readonly property bool capturing: captureState.value !== "idle" && captureState.value !== "error"

    function elapsedText(ms) {
        const s = Math.floor(Number(ms || 0) / 1000)
        const two = n => ("0" + n).slice(-2)
        return Math.floor(s / 3600) + ":" + two(Math.floor(s / 60) % 60) + ":" + two(s % 60)
    }
    // [{label, value}] for an audio stream of the file: nothing, the master or one track.
    function audioChoices() {
        const list = [{ label: "なし", value: "" }, { label: params.value("/master/name") || "マスター", detail: "マスター", value: "master" }]
        for (let i = 0; i < trackList.count; ++i) {
            const n = trackList.numAt(i)
            list.push({ label: routes.trackName(n), detail: "トラック " + n, value: "track/" + n })
        }
        return list
    }

    Flickable {
        id: flick
        objectName: "captureFlick"
        anchors.fill: parent
        contentHeight: column.height + 32
        boundsBehavior: Flickable.StopAtBounds
        clip: true

        Column {
            id: column
            x: 24
            width: Math.min(flick.width - 48, 860)

            SettingSection { text: "録画のファイル" }
            Text {
                width: parent.width
                visible: page.capturing
                wrapMode: Text.Wrap
                font.pixelSize: 13
                color: Theme.warn
                text: "録画中に変えた設定は、次の録画から使われます"
            }
            SettingRow {
                label: "形式"
                note: container.value === "mkv" ? "MKV は PC が落ちても、そこまでの分を再生できます" : "MP4 は録画が異常終了すると、ファイル全体が再生できなくなります"
                ParamSegments { path: "/capture/container"; labels: ({ mkv: "MKV", mp4: "MP4" }) }
            }
            SettingRow {
                label: "保存先フォルダー"
                note: "エンジンの PC のパス。空ならビデオフォルダー"
                ParamText { path: "/capture/directory"; title: "保存先フォルダー"; emptyText: "ビデオフォルダー" }
            }
            SettingRow {
                label: "ファイル名"
                note: "%Y 年 %m 月 %d 日 %H 時 %M 分 %S 秒"
                ParamText { path: "/capture/filename"; title: "ファイル名のパターン" }
            }

            SettingSection { text: "録画の映像" }
            SettingRow {
                label: "録画するミックス"
                ParamPick { path: "/capture/mix"; title: "録画するミックス"; choices: () => Video.mixChoices(true) }
            }
            SettingRow {
                label: "エンコーダー"
                note: "自動は NVENC → AMF → Quick Sync → Media Foundation → x264（CPU）の順に、使えるものを選びます"
                controlWidth: 420
                ParamSegments {
                    path: "/capture/video/encoder"
                    labels: ({ auto: "自動", nvenc: "NVENC", amf: "AMF", qsv: "QSV", x264: "x264" })
                }
            }
            SettingRow {
                label: "コーデック"
                ParamSegments { path: "/capture/video/codec"; labels: ({ h264: "H.264", hevc: "HEVC", av1: "AV1" }) }
            }
            SettingRow {
                label: "解像度"
                note: "ミックスと違う大きさなら拡大・縮小します"
                SizePick { prefix: "/capture/video"; title: "録画の解像度" }
            }
            SettingRow {
                label: "フレームレート"
                ParamPick { path: "/capture/video/fps"; title: "録画のフレームレート"
                            choices: () => [24, 30, 60, 120].map(v => ({ label: v + " fps", value: v })) }
            }
            SettingRow {
                label: "レート制御"
                note: "CBR = 一定、VBR = 可変、CQP = 画質を固定（大きさは決まらない）"
                ParamSegments { path: "/capture/video/rate_control"; labels: ({ cbr: "CBR", vbr: "VBR", cqp: "CQP" }) }
            }
            SettingRow {
                visible: rateControl.value !== "cqp"
                label: "ビットレート"
                controlWidth: 180
                ParamNumber { path: "/capture/video/bitrate_kbps"; title: "ビットレート"; unit: "kbps" }
            }
            SettingRow {
                visible: rateControl.value === "cqp"
                label: "品質"
                note: "小さいほど高画質でファイルが大きい（目安 18〜28）"
                controlWidth: 180
                ParamNumber { path: "/capture/video/quality"; title: "品質値" }
            }

            SettingSection { text: "録画の音声（ストリーム 1〜8）" }
            Text {
                width: parent.width
                wrapMode: Text.Wrap
                color: Theme.subText
                font.pixelSize: 13
                text: "動画ファイルに入れる音声。トラックを別のストリームにすると、編集ソフトで別々に調整できます。"
            }
            Repeater {
                model: 8
                delegate: Item {
                    id: stream
                    required property int index
                    readonly property string base: "/capture/audio/" + (index + 1)
                    width: column.width
                    height: Theme.touch + 12
                    Param { id: aSource; path: stream.base + "/source" }
                    Param { id: aCodec; path: stream.base + "/codec" }
                    Text {
                        id: streamLabel
                        anchors { left: parent.left; verticalCenter: parent.verticalCenter }
                        width: 40
                        color: Theme.text
                        font.pixelSize: 15
                        text: stream.index + 1
                    }
                    ParamPick {
                        objectName: "audioStream_" + (stream.index + 1)
                        anchors { left: streamLabel.right; right: codecRow.left; rightMargin: 8; verticalCenter: parent.verticalCenter }
                        width: undefined
                        path: stream.base + "/source"
                        title: "ストリーム " + (stream.index + 1) + " の音声"
                        choices: page.audioChoices
                    }
                    Row {
                        id: codecRow
                        anchors { right: parent.right; verticalCenter: parent.verticalCenter }
                        spacing: 6
                        opacity: aSource.value ? 1 : 0.4
                        ParamSegments {
                            width: Math.min(240, column.width * 0.35)
                            path: stream.base + "/codec"
                            labels: ({ aac: "AAC", opus: "Opus", pcm: "PCM" })
                        }
                        ParamNumber {
                            width: 110
                            visible: aCodec.value !== "pcm"
                            path: stream.base + "/bitrate_kbps"
                            title: "音声のビットレート"
                            unit: "kbps"
                        }
                        Item { width: 110; height: 1; visible: aCodec.value === "pcm" }
                    }
                }
            }

            SettingSection { text: "トラック別録音（WAV / FLAC）" }
            Item {
                width: parent.width
                height: Theme.touch + 16
                ToggleButton {
                    id: recButton
                    objectName: "recorderButton"
                    anchors { left: parent.left; verticalCenter: parent.verticalCenter }
                    width: 160
                    label: recState.value === "recording" ? "■ 録音停止" : "● 録音開始"
                    on: recState.value === "recording"
                    onColor: Theme.record
                    onTapped: params.call(recState.value === "recording" ? "/recorder/stop" : "/recorder/start")
                }
                Column {
                    anchors { left: recButton.right; leftMargin: 16; right: parent.right; verticalCenter: parent.verticalCenter }
                    Text {
                        width: parent.width
                        elide: Text.ElideMiddle
                        color: recState.value === "error" ? Theme.mute : recState.value === "recording" ? Theme.record : Theme.text
                        font.pixelSize: 15
                        text: recState.value === "recording" ? "録音中 " + page.elapsedText(recElapsed.value)
                            : recState.value === "error" ? "エラー: " + recError.value : "停止中"
                    }
                    Text {
                        width: parent.width
                        visible: text.length > 0
                        elide: Text.ElideMiddle
                        color: Theme.subText
                        font.pixelSize: 12
                        text: (recFolder.value || "") + (recDropped.value > 0 ? "  書き込み遅れ " + recDropped.value + " フレーム" : "")
                    }
                }
            }
            Text {
                width: parent.width
                wrapMode: Text.Wrap
                color: Theme.subText
                font.pixelSize: 13
                text: "録音するトラック（ミキサーの REC、またはここでタップ）:"
            }
            Flow {
                width: parent.width
                spacing: 6
                topPadding: 6
                bottomPadding: 6
                ToggleButton {
                    objectName: "armMaster"
                    width: 140
                    fontSize: 14
                    label: params.value("/master/name") || "マスター"
                    on: masterArmed.value === true
                    onColor: Theme.record
                    onTapped: masterArmed.set(!masterArmed.value)
                }
                Repeater {
                    model: trackList
                    delegate: ToggleButton {
                        id: arm
                        required property int num
                        required property string base
                        objectName: "arm_" + num
                        width: 140
                        fontSize: 14
                        Param { id: armed; path: arm.base + "/record/enabled" }
                        Param { id: armName; path: arm.base + "/name" }
                        label: armName.value || "トラック " + arm.num
                        on: armed.value === true
                        onColor: Theme.record
                        onTapped: armed.set(!armed.value)
                    }
                }
            }
            SettingRow {
                label: "録画と一緒に"
                note: "録画の開始・停止に合わせて録音も開始・停止する"
                controlWidth: 100
                ParamSwitch { path: "/recorder/with_capture" }
            }
            SettingRow {
                label: "形式"
                note: "48kHz / 24bit。WAV は 4GB を超えると RF64 になります"
                ParamSegments { path: "/recorder/format"; labels: ({ wav: "WAV", flac: "FLAC" }) }
            }
            SettingRow {
                label: "保存先フォルダー"
                note: "空ならミュージック\\Desktop Capture"
                ParamText { path: "/recorder/directory"; title: "録音の保存先"; emptyText: "ミュージック\\Desktop Capture" }
            }
            SettingRow {
                label: "フォルダー名"
                note: "録音ごとに作るフォルダーの名前（%Y %m %d %H %M %S）"
                ParamText { path: "/recorder/folder_name"; title: "フォルダー名のパターン" }
            }
        }
    }
}
