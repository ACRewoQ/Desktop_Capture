import QtQuick
import DcUi.Params

// Profiles and mixer snapshots (protocol.md 5.10): the profile files of the engine PC (switch, save
// as, new, rename, delete, autosave) and the snapshots of the current profile (take, recall with one
// tap, overwrite, rename, delete).
Item {
    id: page
    readonly property bool swipeable: true

    Param { id: current; path: "/profile/current" }
    Param { id: modified; path: "/profile/modified" }
    Param { id: autosave; path: "/profile/autosave" }
    Param { id: lastError; path: "/profile/error" }
    Param { id: directory; path: "/profile/directory" }
    Param { id: lastSnapshot; path: "/snapshot/last" }
    Param { id: captureState; path: "/capture/state" }
    Param { id: recorderState; path: "/recorder/state" }
    ParamList { id: profiles; prefix: "/profile/list" }
    ParamList { id: snapshots; prefix: "/snapshot" }

    readonly property bool enabled_: directory.value !== undefined && directory.value !== ""
    readonly property bool busy: (captureState.value !== "idle" && captureState.value !== "error" && captureState.value !== undefined) ||
                                 recorderState.value === "recording"

    function timeText(t) {
        if (!t) return ""
        return Qt.formatDateTime(new Date(t * 1000), "yyyy/MM/dd hh:mm")
    }
    function askName(title, value, cb) {
        Sheets.text.open(title, value, (t) => { if (t !== "") cb(t) })
    }
    function profileMenu(name) {
        const isCurrent = name === current.value
        const list = [
            { label: "切り替える", value: "load", disabled: isCurrent || page.busy,
              detail: isCurrent ? "使用中" : page.busy ? "録画中・録音中は切り替えられません" : "今の設定は保存してから切り替えます" },
            { label: "名前を変える", value: "rename" },
            { label: "削除する", value: "remove", disabled: isCurrent, detail: isCurrent ? "使用中のものは削除できません" : "" },
        ]
        Sheets.choice.open(name, list, (c) => {
            if (c.disabled) return
            if (c.value === "load") params.call("/profile/load", { name: name })
            else if (c.value === "rename")
                askName("新しい名前", name, (t) => params.call("/profile/rename", { name: name, new_name: t }))
            else if (c.value === "remove")
                Sheets.choice.open(name + " を削除しますか", [{ label: "削除する", value: "yes", detail: "元に戻せません" },
                                                             { label: "やめる", value: "no" }], (d) => {
                    if (d.value === "yes") params.call("/profile/remove", { name: name })
                })
        })
    }
    function snapshotMenu(n, name) {
        Sheets.choice.open(name, [
            { label: "呼び出す", value: "recall" },
            { label: "今の値で上書き", value: "save" },
            { label: "名前を変える", value: "rename" },
            { label: "削除する", value: "remove" },
        ], (c) => {
            if (c.value === "recall") params.call("/snapshot/recall", { snapshot: n })
            else if (c.value === "save") params.call("/snapshot/save", { snapshot: n })
            else if (c.value === "rename") askName("スナップショットの名前", name, (t) => params.set("/snapshot/" + n + "/name", t))
            else if (c.value === "remove") params.call("/snapshot/remove", { snapshot: n })
        })
    }

    Flickable {
        id: flick
        objectName: "profileFlick"
        anchors.fill: parent
        contentHeight: column.height + 32
        boundsBehavior: Flickable.StopAtBounds
        clip: true

        Column {
            id: column
            x: 24
            width: Math.min(flick.width - 48, 900)

            SettingSection { text: "プロファイル" }
            Text {
                width: parent.width
                wrapMode: Text.Wrap
                color: Theme.subText
                font.pixelSize: 13
                text: "ミキサー・エフェクト・サンプラー・映像・録画の設定をまとめて保存し、切り替えます。エンジンを起動すると前回のプロファイルに戻ります。ホットキー・MIDI の割り当てとペアリングはどのプロファイルでも共通です。"
            }
            InfoRow {
                objectName: "profileCurrent"
                label: "使用中"
                value: !page.enabled_ ? "（このエンジンではプロファイルを使っていません）"
                       : (current.value || "—") + (modified.value ? "　未保存の変更あり" : "　保存済み")
            }
            Text {
                visible: !!lastError.value
                width: parent.width
                wrapMode: Text.Wrap
                topPadding: 4
                color: Theme.mute
                font.pixelSize: 13
                text: lastError.value || ""
            }
            Item { width: 1; height: 8 }
            Flow {
                width: parent.width
                spacing: 8
                visible: page.enabled_
                MenuButton {
                    objectName: "profileSave"
                    width: 200; height: Theme.touch
                    label: "今すぐ保存"
                    onTapped: params.call("/profile/save")
                }
                MenuButton {
                    objectName: "profileSaveAs"
                    width: 200; height: Theme.touch
                    label: "別名で保存"
                    onTapped: page.askName("別名で保存", (current.value || "") + " のコピー",
                                           (t) => params.call("/profile/save", { name: t }))
                }
                MenuButton {
                    objectName: "profileNew"
                    width: 200; height: Theme.touch
                    label: "＋ 新しく作る"
                    active: !page.busy
                    onTapped: page.askName("新しいプロファイル（初期状態から）", "", (t) => params.call("/profile/new", { name: t }))
                }
            }
            SettingRow {
                visible: page.enabled_
                label: "自動保存"
                note: "変更すると 2 秒後に使用中のプロファイルへ保存します"
                controlWidth: 100
                ParamSwitch { path: "/profile/autosave" }
            }
            Item { width: 1; height: 4 }
            Repeater {
                model: profiles
                delegate: Rectangle {
                    id: prow
                    required property int num
                    required property string base
                    objectName: "profile_" + num
                    Param { id: pName; path: prow.base + "/name" }
                    Param { id: pTime; path: prow.base + "/modified" }
                    readonly property bool isCurrent: pName.value === current.value
                    width: column.width
                    height: 52
                    color: ptap.pressed ? Theme.controlPressed : (num % 2 ? Theme.stripA : Theme.stripB)
                    radius: Theme.radius
                    border.width: isCurrent ? 2 : 0
                    border.color: Theme.accent
                    Text {
                        x: 12
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width - timeText.width - 36
                        elide: Text.ElideRight
                        color: Theme.text
                        font.pixelSize: 16
                        font.bold: prow.isCurrent
                        text: (pName.value || "") + (prow.isCurrent ? "　（使用中）" : "")
                    }
                    Text {
                        id: timeText
                        anchors { right: parent.right; rightMargin: 12; verticalCenter: parent.verticalCenter }
                        color: Theme.subText
                        font.pixelSize: 13
                        text: page.timeText(pTime.value)
                    }
                    TapHandler { id: ptap; onTapped: page.profileMenu(pName.value) }
                }
            }
            Text {
                visible: page.enabled_ && profiles.count === 0
                color: Theme.subText
                font.pixelSize: 14
                text: "まだ保存したプロファイルがありません（最初の変更で「" + (current.value || "") + "」が保存されます）"
            }

            SettingSection { text: "ミキサースナップショット" }
            Text {
                width: parent.width
                wrapMode: Text.Wrap
                color: Theme.subText
                font.pixelSize: 13
                text: "フェーダー・パン・ミュート・ソロ・Lo-Hi・センド量・エフェクトの値を撮っておき、タップで戻します。入力や出力先、トラックの数は変わりません。ホットキー・MIDI からも呼び出せます（キー・MIDI のページ）。"
            }
            Item { width: 1; height: 8 }
            MenuButton {
                objectName: "snapshotSave"
                width: 240
                height: Theme.touch
                label: "＋ 今の値を撮る"
                onTapped: params.call("/snapshot/save")
            }
            Item { width: 1; height: 8 }
            Text {
                visible: snapshots.count === 0
                color: Theme.subText
                font.pixelSize: 14
                text: "まだスナップショットがありません"
            }
            Grid {
                id: snapGrid
                width: parent.width
                columns: Math.max(1, Math.floor((width + 8) / 228))
                spacing: 8
                Repeater {
                    model: snapshots
                    delegate: Rectangle {
                        id: srow
                        required property int num
                        required property string base
                        objectName: "snapshot_" + num
                        Param { id: sName; path: srow.base + "/name" }
                        Param { id: sTime; path: srow.base + "/modified" }
                        readonly property bool isLast: lastSnapshot.value === num
                        width: (snapGrid.width - (snapGrid.columns - 1) * 8) / snapGrid.columns
                        height: 72
                        radius: Theme.radius
                        color: stap.pressed ? Theme.controlPressed : Theme.control
                        border.width: isLast ? 2 : 0
                        border.color: Theme.accent
                        Text {
                            x: 12
                            y: 12
                            width: parent.width - 24 - more.width
                            elide: Text.ElideRight
                            color: Theme.text
                            font.pixelSize: 16
                            font.bold: true
                            text: sName.value || ("Snapshot " + srow.num)
                        }
                        Text {
                            x: 12
                            y: 42
                            color: Theme.subText
                            font.pixelSize: 12
                            text: srow.isLast ? "最後に使った" : page.timeText(sTime.value)
                        }
                        Item {
                            anchors { left: parent.left; top: parent.top; bottom: parent.bottom; right: more.left }
                            TapHandler {
                                id: stap
                                onTapped: params.call("/snapshot/recall", { snapshot: srow.num })
                                onLongPressed: page.snapshotMenu(srow.num, sName.value || ("Snapshot " + srow.num))
                            }
                        }
                        Rectangle {
                            id: more
                            objectName: "snapshotMore_" + srow.num
                            anchors { right: parent.right; top: parent.top; bottom: parent.bottom }
                            width: 48
                            radius: Theme.radius
                            color: mtap.pressed ? Theme.controlPressed : "transparent"
                            Text { anchors.centerIn: parent; text: "…"; color: Theme.text; font.pixelSize: 22 }
                            TapHandler { id: mtap; onTapped: page.snapshotMenu(srow.num, sName.value || ("Snapshot " + srow.num)) }
                        }
                    }
                }
            }
            InfoRow {
                visible: page.enabled_
                label: "保存先（エンジンの PC）"
                value: directory.value || ""
            }
        }
    }
}
