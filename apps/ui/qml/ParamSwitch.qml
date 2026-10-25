import QtQuick
import DcUi.Params

// Bool parameter as an on / off switch.
Item {
    id: root
    property alias path: param.path
    readonly property alias param: param
    width: parent ? parent.width : 0
    height: Theme.touch
    Param { id: param }

    Rectangle {
        objectName: "switch"
        anchors { right: parent.right; verticalCenter: parent.verticalCenter }
        width: 72
        height: 36
        radius: 18
        color: param.value ? Theme.accentDim : Theme.control
        opacity: param.readOnly ? 0.5 : 1
        Rectangle {
            width: 28; height: 28; radius: 14
            y: 4
            x: param.value ? parent.width - width - 4 : 4
            color: param.value ? Theme.accent : "#c8ccd2"
            Behavior on x { NumberAnimation { duration: 120 } }
        }
    }
    TapHandler { enabled: !param.readOnly; onTapped: param.set(!param.value) }
}
