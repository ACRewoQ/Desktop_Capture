import QtQuick

// Remote controller, first connection to an engine: the 6 digits shown on the engine's LAN page.
Rectangle {
    id: root
    property string code: ""
    property string message
    signal entered(string code)

    function key(k) {
        if (k === "⌫") code = code.slice(0, -1)
        else if (code.length < 6) code += k
        if (code.length === 6) { const c = code; code = ""; entered(c) }
    }

    width: 360
    height: column.height + 40
    radius: 10
    color: Theme.panel
    border.color: Theme.border

    Column {
        id: column
        x: 20; y: 20
        width: parent.width - 40
        spacing: 12
        Text {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            color: Theme.text
            font.pixelSize: 18
            text: "ペアリング"
        }
        Text {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            color: Theme.subText
            font.pixelSize: 13
            text: root.message + "\nエンジンの PC の LAN ページで「受付を始める」を押し、出た 6 桁の数字を入れてください。"
        }
        Row {
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: 8
            Repeater {
                model: 6
                delegate: Rectangle {
                    required property int index
                    width: 40; height: 52; radius: 6
                    color: Theme.background
                    border.color: index === root.code.length ? Theme.accent : Theme.border
                    Text {
                        anchors.centerIn: parent
                        color: Theme.text
                        font.pixelSize: 26
                        text: root.code.charAt(index)
                    }
                }
            }
        }
        Grid {
            anchors.horizontalCenter: parent.horizontalCenter
            columns: 3
            spacing: 8
            Repeater {
                model: ["1", "2", "3", "4", "5", "6", "7", "8", "9", "", "0", "⌫"]
                delegate: MenuButton {
                    required property string modelData
                    objectName: "pairKey_" + modelData
                    width: 96
                    height: 56
                    fontSize: 22
                    opacity: modelData !== "" ? 1 : 0
                    active: modelData !== ""
                    label: modelData
                    onTapped: root.key(modelData)
                }
            }
        }
    }
}
