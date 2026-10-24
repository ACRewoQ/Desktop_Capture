import QtQuick
import DcUi.Params

// The sources shared by every scene and mix: what each one is, whether capd has it open, and add.
Item {
    id: root
    signal sourceRequested(int source)

    function add() {
        Sheets.choice.open("追加するソースの種類", Video.kinds, c => {
            params.call("/video/source/add", { kind: c.value }, (ok, n) => { if (ok) root.sourceRequested(n) })
        })
    }

    ParamList { id: sources; prefix: "/video/source" }

    Flickable {
        id: flick
        objectName: "sourcesFlick"
        anchors.fill: parent
        contentHeight: column.height + 24
        boundsBehavior: Flickable.StopAtBounds
        clip: true
        Column {
            id: column
            x: 12
            width: Math.min(flick.width - 24, 860)
            spacing: 6
            Text {
                width: parent.width
                wrapMode: Text.Wrap
                color: Theme.subText
                font.pixelSize: 13
                text: "ソースはすべてのシーンで共有します。どのシーンにも出ていないソースは取り込みを休みます。"
            }
            Repeater {
                model: sources
                delegate: Rectangle {
                    id: row
                    required property int num
                    required property string base
                    objectName: "source_" + num
                    width: column.width
                    height: 56
                    radius: Theme.radius
                    color: tap.pressed ? Theme.controlPressed : Theme.control
                    Param { id: sName; path: row.base + "/name" }
                    Param { id: sKind; path: row.base + "/kind" }
                    Param { id: sState; path: row.base + "/state" }
                    Param { id: sWidth; path: row.base + "/width" }
                    Param { id: sHeight; path: row.base + "/height" }
                    Param { id: sError; path: row.base + "/error" }
                    Rectangle {
                        id: kindBadge
                        anchors { left: parent.left; leftMargin: 8; verticalCenter: parent.verticalCenter }
                        width: 84; height: 32; radius: 4
                        color: Theme.stripA
                        Text { anchors.centerIn: parent; color: Theme.text; font.pixelSize: 13; text: Video.kindLabel(sKind.value) }
                    }
                    Column {
                        anchors { left: kindBadge.right; right: stateText.left; leftMargin: 10; rightMargin: 8; verticalCenter: parent.verticalCenter }
                        Text {
                            width: parent.width
                            elide: Text.ElideRight
                            color: "white"
                            font.pixelSize: 15
                            text: sName.value || Video.kindLabel(sKind.value) + " " + row.num
                        }
                        Text {
                            width: parent.width
                            visible: text.length > 0
                            elide: Text.ElideRight
                            color: sState.value === "error" ? Theme.mute : Theme.subText
                            font.pixelSize: 11
                            text: sState.value === "error" ? sError.value : Video.sizeText(sWidth.value, sHeight.value)
                        }
                    }
                    Text {
                        id: stateText
                        anchors { right: parent.right; rightMargin: 12; verticalCenter: parent.verticalCenter }
                        color: Video.stateColor(sState.value)
                        font.pixelSize: 13
                        text: Video.stateText(sState.value)
                    }
                    TapHandler { id: tap; onTapped: root.sourceRequested(row.num) }
                }
            }
            MenuButton {
                objectName: "addSource"
                width: parent.width
                height: Theme.touch
                label: "+ ソースを追加"
                active: sources.count < 64
                onTapped: root.add()
            }
        }
    }
}
