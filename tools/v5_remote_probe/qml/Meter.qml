import QtQuick

Rectangle {
    property real level: 0 // 0..1

    color: "#111317"
    Rectangle {
        anchors.bottom: parent.bottom
        width: parent.width
        height: parent.height * parent.level
        color: parent.level > 0.92 ? "#ef5350" : parent.level > 0.75 ? "#ffca28" : "#66bb6a"
    }
}
