import QtQuick
import DcUi.Params

// Number parameter: shows the value; a tap opens the keypad (range from the schema).
Rectangle {
    id: root
    property alias path: param.path
    property string title
    property string unit
    property int decimals: param.type === "int" ? 0 : 1
    property var format: null // function(value) -> text, instead of the plain number
    property real factor: 1   // shown and typed as value * factor (fractions as percent: 100)
    readonly property alias param: param

    width: parent ? parent.width : 0
    height: Theme.touch
    radius: Theme.radius
    color: tap.pressed ? Theme.controlPressed : Theme.control
    opacity: param.readOnly ? 0.5 : 1
    Param { id: param }

    Text {
        anchors.centerIn: parent
        color: "white"
        font.pixelSize: 15
        text: root.format ? root.format(param.value)
                          : (Number(param.value || 0) * root.factor).toFixed(root.decimals) + (root.unit ? " " + root.unit : "")
    }
    TapHandler {
        id: tap
        enabled: !param.readOnly
        onTapped: Sheets.number.open({ title: root.title, value: Number(param.value) * root.factor, unit: root.unit,
                                       min: param.min * root.factor, max: param.max * root.factor, decimals: root.decimals },
                                     v => param.set(v / root.factor))
    }
}
