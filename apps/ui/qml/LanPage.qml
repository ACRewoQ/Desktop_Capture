import QtQuick
import DcUi.Params

// LAN (protocol.md 8 and 10): this PC's name, accepting pairings (remote controllers and other PCs),
// the other PCs this one receives from, the audio streams it sends, and who receives them.
Item {
    id: page
    readonly property bool swipeable: true
    property string pairCode: ""

    Param { id: lanId; path: "/lan/id" }
    Param { id: pairing; path: "/remote/pairing/active" }
    Param { id: remoteClients; path: "/remote/clients" }
    ParamList { id: paired; prefix: "/remote/paired" }
    ParamList { id: peers; prefix: "/lan/peers" }
    ParamList { id: subs; prefix: "/lan/sub" }

    function openDemo(name) { if (name === "pairing") startPairing() }
    function startPairing() {
        params.call("/remote/pairing/start", {}, (ok, v) => { if (ok) page.pairCode = String(v) })
    }
    function peerStateText(s) {
        return s === "connected" ? "接続中" : s === "connecting" ? "接続しています…" : s === "unpaired" ? "ペアリング前"
             : s === "refused" ? "相手が取り消しました" : "見つかりません"
    }
    function peerStateColor(s) {
        return s === "connected" ? Theme.ok : s === "connecting" ? Theme.warn : s === "refused" ? Theme.mute : Theme.subText
    }
    function pairPeer(id, name) {
        Sheets.text.open(name + " に表示された 6 桁の数字", "", t => {
            const code = t.replace(/\D/g, "")
            if (code.length !== 6) { Sheets.toast.show("6 桁の数字を入れてください"); return }
            params.call("/lan/peers/pair", { id: id, code: code })
        })
    }
    function confirm(title, detail, action) {
        Sheets.choice.open(title, [{ label: "はい", detail: detail, value: true }, { label: "やめる", value: false }],
                           c => { if (c.value) action() })
    }

    Flickable {
        id: flick
        objectName: "lanFlick"
        anchors.fill: parent
        contentHeight: column.height + 32
        boundsBehavior: Flickable.StopAtBounds
        clip: true

        Column {
            id: column
            x: 24
            width: Math.min(flick.width - 48, 860)

            SettingSection { text: "この PC" }
            SettingRow {
                label: "LAN での名前"
                note: "ほかの PC やリモコンの一覧に出る名前。空ならコンピューター名"
                ParamText { path: "/lan/name"; title: "LAN での名前"; emptyText: "（コンピューター名）" }
            }
            InfoRow { label: "ID"; value: lanId.value || "—" }

            SettingSection { text: "つないでくる機器を受け付ける" }
            Text {
                width: parent.width
                wrapMode: Text.Wrap
                color: Theme.subText
                font.pixelSize: 13
                text: "リモコンや別の PC がこの PC につなぐには、最初に 1 回ペアリングが要ります。受付を始めて、相手の画面に下の数字を入れてください（2 分で終わります）。"
            }
            Item {
                width: parent.width
                height: pairing.value ? 96 : Theme.touch + 16
                MenuButton {
                    objectName: "pairingButton"
                    anchors { left: parent.left; verticalCenter: parent.verticalCenter }
                    width: 200
                    height: Theme.touch
                    label: pairing.value ? "受付をやめる" : "受付を始める"
                    onTapped: pairing.value ? params.call("/remote/pairing/stop") : page.startPairing()
                }
                Text {
                    objectName: "pairCode"
                    visible: pairing.value === true && page.pairCode.length > 0
                    anchors { left: parent.left; leftMargin: 224; verticalCenter: parent.verticalCenter }
                    color: Theme.accent
                    font.pixelSize: 48
                    font.bold: true
                    font.letterSpacing: 8
                    text: page.pairCode.substring(0, 3) + " " + page.pairCode.substring(3)
                }
            }
            Text {
                visible: paired.count > 0
                topPadding: 6
                color: Theme.subText
                font.pixelSize: 13
                text: "ペアリング済み（リモコン " + remoteClients.value + " 台が接続中）"
            }
            Repeater {
                model: paired
                delegate: SettingRow {
                    id: pairedRow
                    required property string base
                    Param { id: pId; path: pairedRow.base + "/id" }
                    Param { id: pName; path: pairedRow.base + "/name" }
                    Param { id: pRole; path: pairedRow.base + "/role" }
                    Param { id: pOnline; path: pairedRow.base + "/online" }
                    label: (pName.value || pId.value) + "（" + (pRole.value === "peer" ? "別の PC" : "リモコン") + "）"
                    note: pOnline.value ? "接続中" : "未接続"
                    controlWidth: 140
                    MenuButton {
                        width: parent.width
                        height: Theme.touch
                        fontSize: 14
                        label: "取り消す"
                        onTapped: {
                            const id = pId.value
                            page.confirm((pName.value || id) + " のペアリングを取り消しますか", "つながっていれば切れます。もう一度使うにはペアリングし直します",
                                         () => params.call("/remote/unpair", { id: id }))
                        }
                    }
                }
            }

            SettingSection { text: "別の PC から受け取る" }
            Text {
                width: parent.width
                wrapMode: Text.Wrap
                color: Theme.subText
                font.pixelSize: 13
                text: "同じ LAN のこのアプリは自動で見つかります。相手の PC で受付を始め、ここで「ペアリング」を押して数字を入れると、相手の LAN 送信をトラックの入力に選べます。"
            }
            Repeater {
                model: peers
                delegate: Column {
                    id: peer
                    required property int num
                    required property string base
                    objectName: "peer_" + num
                    width: column.width
                    Param { id: peerId; path: peer.base + "/id" }
                    Param { id: peerName; path: peer.base + "/name" }
                    Param { id: peerAddress; path: peer.base + "/address" }
                    Param { id: peerState; path: peer.base + "/state" }
                    Param { id: peerManual; path: peer.base + "/manual" }
                    ParamList { id: peerStreams; prefix: peer.base + "/stream" }
                    SettingRow {
                        label: peerName.value || peerAddress.value
                        note: peerAddress.value + (peerManual.value ? "・手動で追加" : "")
                        controlWidth: 360
                        Row {
                            spacing: 6
                            Text {
                                width: 150
                                height: Theme.touch
                                verticalAlignment: Text.AlignVCenter
                                horizontalAlignment: Text.AlignRight
                                color: page.peerStateColor(peerState.value)
                                font.pixelSize: 14
                                text: page.peerStateText(peerState.value)
                            }
                            MenuButton {
                                objectName: "pairPeer_" + peer.num
                                width: 110; height: Theme.touch; fontSize: 14
                                visible: peerState.value === "unpaired" || peerState.value === "refused"
                                label: "ペアリング"
                                onTapped: page.pairPeer(peerId.value, peerName.value || peerAddress.value)
                            }
                            MenuButton {
                                width: 88; height: Theme.touch; fontSize: 14
                                label: peerManual.value ? "削除" : "忘れる"
                                onTapped: {
                                    const id = peerId.value
                                    page.confirm((peerName.value || peerAddress.value) + " を" + (peerManual.value ? "一覧から消しますか" : "忘れますか"),
                                                 "ペアリング情報を消します。この相手の音を入力にしているトラックは無音になります",
                                                 () => params.call("/lan/peers/forget", { id: id }))
                                }
                            }
                        }
                    }
                    // The peer's send streams (only while connected): the ones with sound stand out.
                    Flow {
                        width: parent.width
                        visible: peerStreams.count > 0
                        spacing: 6
                        bottomPadding: 10
                        Repeater {
                            model: peerStreams
                            delegate: Rectangle {
                                id: ps
                                required property int num
                                required property string base
                                Param { id: psName; path: ps.base + "/name" }
                                Param { id: psActive; path: ps.base + "/active" }
                                width: psText.implicitWidth + 20
                                height: 30
                                radius: 15
                                color: psActive.value ? Theme.accentDim : Theme.stripA
                                Text {
                                    id: psText
                                    anchors.centerIn: parent
                                    color: psActive.value ? "white" : Theme.subText
                                    font.pixelSize: 13
                                    text: (psName.value || "LAN " + ps.num) + (psActive.value ? " ♪" : "")
                                }
                            }
                        }
                    }
                }
            }
            Text {
                visible: peers.count === 0
                topPadding: 8
                color: Theme.subText
                font.pixelSize: 14
                text: "見つかった PC はありません"
            }
            Item { width: 1; height: 8 }
            MenuButton {
                objectName: "addPeer"
                width: 260
                height: Theme.touch
                fontSize: 15
                label: "IP アドレスで追加…"
                onTapped: Sheets.text.open("相手の IP アドレス（例 192.168.1.20）", "", t => { if (t) params.call("/lan/peers/add", { address: t }) })
            }

            SettingSection { text: "LAN 送信（この PC の音を送る）" }
            Text {
                width: parent.width
                wrapMode: Text.Wrap
                color: Theme.subText
                font.pixelSize: 13
                text: "トラックやマスターの出力先に「LAN 1〜8」を選ぶと、受け取りを申し込んだ相手に送ります。無圧縮（PCM）は遅延が小さく、Opus は無線 LAN 向けです。"
            }
            Repeater {
                model: 8
                delegate: Column {
                    id: stream
                    required property int index
                    readonly property string base: "/lan/stream/" + (index + 1)
                    width: column.width
                    Param { id: sName; path: stream.base + "/name" }
                    Param { id: sCodec; path: stream.base + "/codec" }
                    Param { id: sSubscribers; path: stream.base + "/subscribers" }
                    Param { id: sActive; path: stream.base + "/active" }
                    SettingRow {
                        label: (sName.value || "LAN " + (stream.index + 1))
                        note: sActive.value ? "送信中（" + sSubscribers.value + " 台）"
                            : sSubscribers.value > 0 ? sSubscribers.value + " 台が受信待ち（出力先に選ばれていません）" : "受け取る相手なし"
                        controlWidth: 420
                        Row {
                            spacing: 6
                            ParamText {
                                width: 140
                                path: stream.base + "/name"
                                title: "LAN " + (stream.index + 1) + " の名前"
                                emptyText: "LAN " + (stream.index + 1)
                            }
                            ParamSegments {
                                width: 274
                                path: stream.base + "/codec"
                                labels: ({ pcm16: "PCM 16", pcm24: "PCM 24", opus: "Opus" })
                            }
                        }
                    }
                    SettingRow {
                        visible: sCodec.value === "opus"
                        label: "　Opus の設定"
                        note: "パケットが短いほど遅延が小さく、通信量が増えます"
                        controlWidth: 420
                        Row {
                            spacing: 6
                            ParamPick {
                                width: 200
                                path: stream.base + "/opus_frames"
                                title: "パケット長"
                                choices: () => [120, 240, 480, 960].map(v => ({ label: (v / 48).toFixed(1) + " ms", value: v }))
                            }
                            ParamNumber { width: 214; path: stream.base + "/bitrate_kbps"; title: "Opus のビットレート"; unit: "kbps" }
                        }
                    }
                }
            }

            SettingSection { text: "受け取っている相手" }
            Repeater {
                model: subs
                delegate: InfoRow {
                    id: sub
                    required property string base
                    Param { id: subStream; path: sub.base + "/stream" }
                    Param { id: subClient; path: sub.base + "/client" }
                    Param { id: subAddress; path: sub.base + "/address" }
                    label: subClient.value || subAddress.value
                    note: subAddress.value
                    value: String(subStream.value).startsWith("audio/") ? "LAN " + String(subStream.value).substring(6)
                         : subStream.value === "video/preview" ? "映像プレビュー" : subStream.value === "video/next" ? "シーン画面の映像" : subStream.value === "video/lan" ? "映像の LAN 送信" : subStream.value
                }
            }
            Text {
                visible: subs.count === 0
                topPadding: 8
                color: Theme.subText
                font.pixelSize: 14
                text: "いません"
            }
        }
    }
}
