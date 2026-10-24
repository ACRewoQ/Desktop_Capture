import QtQuick
import DcUi.Params

// Colour parameter (0xAARRGGBB, or 0xRRGGBB when alpha is false) as a swatch; a tap opens the palette.
Rectangle {
    id: root
    property alias path: param.path
    property string title: "色"
    property bool alpha: true
    readonly property alias param: param

    width: parent ? parent.width : 0
    height: Theme.touch
    radius: Theme.radius
    color: Theme.control
    Param { id: param }

    readonly property real v: Number(param.value) || 0
    readonly property bool clear: alpha && v < 0x0d000000 // under 5 % opaque

    Rectangle {
        anchors { fill: parent; margins: 6 }
        radius: 4
        color: "#6b7280"
        // Two halves under the colour so that a see-through colour shows as such.
        Rectangle {
            anchors { top: parent.top; bottom: parent.bottom; right: parent.right }
            width: parent.width / 2
            radius: 4
            color: "#c8ccd2"
        }
        Rectangle {
            anchors.fill: parent
            radius: 4
            border.color: "#40ffffff"
            color: root.alpha ? "#" + ("0000000" + Math.round(root.v).toString(16)).slice(-8)
                              : "#" + ("00000" + Math.round(root.v).toString(16)).slice(-6)
        }
        Text {
            anchors.centerIn: parent
            visible: root.clear
            color: "white"
            style: Text.Outline
            styleColor: "black"
            font.pixelSize: 13
            text: "透明"
        }
    }
    TapHandler {
        onTapped: Sheets.color.open({ title: root.title, value: param.value, alpha: root.alpha }, v => param.set(v))
    }
}
