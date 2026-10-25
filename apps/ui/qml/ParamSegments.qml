import QtQuick
import DcUi.Params

// Choice parameter as a row of buttons (few choices). labels maps a value to its text.
Row {
    id: root
    property alias path: param.path
    property var labels: ({})
    property var values: param.choices
    readonly property alias param: param

    width: parent ? parent.width : 0
    spacing: 4
    Param { id: param }

    Repeater {
        model: root.values
        delegate: Rectangle {
            required property var modelData
            required property int index
            readonly property bool current: param.value === modelData
            objectName: "seg_" + String(modelData)
            width: (root.width - root.spacing * (root.values.length - 1)) / root.values.length
            height: Theme.touch
            radius: Theme.radius
            color: current ? Theme.accentDim : tap.pressed ? Theme.controlPressed : Theme.control
            border.width: current ? 1 : 0
            border.color: Theme.accent
            opacity: param.readOnly ? 0.5 : 1
            Text {
                anchors.centerIn: parent
                width: parent.width - 6
                horizontalAlignment: Text.AlignHCenter
                elide: Text.ElideRight
                color: "white"
                font.pixelSize: 14
                text: root.labels[modelData] !== undefined ? root.labels[modelData] : String(modelData)
            }
            TapHandler { id: tap; enabled: !param.readOnly; onTapped: param.set(modelData) }
        }
    }
}
