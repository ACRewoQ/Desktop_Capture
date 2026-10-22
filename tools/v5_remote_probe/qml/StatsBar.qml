import QtQuick

Rectangle {
    height: 36
    color: "#15171b"

    Text {
        anchors { verticalCenter: parent.verticalCenter; left: parent.left; leftMargin: 12 }
        color: "#cfd3d9"
        font.pixelSize: 14
        text: "メモリ " + probe.workingSetMb.toFixed(1) + " MB（専用 " + probe.privateMb.toFixed(1)
              + " / ピーク " + probe.peakWorkingSetMb.toFixed(1) + "）　CPU " + probe.cpuPercent.toFixed(1)
              + "%　" + probe.fps + " fps　操作中フェーダー " + probe.activeFaders + "（最大同時 "
              + probe.maxSimultaneousFaders + "）　" + probe.info + (strips.connected ? "　接続中" : "　模擬")
    }
}
