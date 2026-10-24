import QtQuick
import DcUi.Params

// Parameter picked from a longer list: shows the current entry, a tap opens the list.
// choices() returns [{label, detail?, value}]; it is called when the list opens.
Rectangle {
    id: root
    property alias path: param.path
    property string title
    property var choices: function() { return param.choices.map(c => ({ label: String(c), value: c })) }
    property string emptyText: "なし"
    readonly property alias param: param

    function currentLabel() {
        const list = root.choices()
        for (let i = 0; i < list.length; ++i)
            if (list[i].value === param.value) return list[i].label
        return param.value === "" || param.value === undefined ? emptyText : String(param.value)
    }

    width: parent ? parent.width : 0
    height: Theme.touch
    radius: Theme.radius
    color: tap.pressed ? Theme.controlPressed : Theme.control
    opacity: param.readOnly ? 0.5 : 1
    Param { id: param }

    Text {
        anchors { left: parent.left; right: arrow.left; leftMargin: 12; verticalCenter: parent.verticalCenter }
        elide: Text.ElideRight
        color: "white"
        font.pixelSize: 15
        text: param.value, root.currentLabel()
    }
    Text {
        id: arrow
        anchors { right: parent.right; rightMargin: 12; verticalCenter: parent.verticalCenter }
        color: Theme.subText
        font.pixelSize: 15
        text: "▾"
    }
    TapHandler {
        id: tap
        enabled: !param.readOnly
        onTapped: {
            const list = root.choices().map(c => Object.assign({ selected: c.value === param.value }, c))
            Sheets.choice.open(root.title, list, c => param.set(c.value))
        }
    }
}
