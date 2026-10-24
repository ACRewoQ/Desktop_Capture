import QtQuick

// Page buttons on the left edge. pages: [{key, label}].
Rectangle {
    id: root
    property var pages: []
    property string current
    signal selected(string key)

    width: Theme.navWidth
    color: Theme.nav

    Flickable {
        anchors.fill: parent
        contentHeight: column.height
        interactive: contentHeight > height
        boundsBehavior: Flickable.StopAtBounds
        Column {
            id: column
            width: parent.width
            Repeater {
                model: root.pages
                delegate: Rectangle {
                    required property var modelData
                    readonly property bool isCurrent: root.current === modelData.key
                    objectName: "nav_" + modelData.key
                    width: column.width
                    height: 68
                    color: tap.pressed ? Theme.controlPressed : isCurrent ? Theme.stripA : "transparent"
                    Rectangle {
                        visible: parent.isCurrent
                        anchors { left: parent.left; top: parent.top; bottom: parent.bottom; margins: 10; leftMargin: 0 }
                        width: 4
                        color: Theme.accent
                    }
                    Text {
                        anchors.centerIn: parent
                        width: parent.width - 12
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.Wrap
                        color: parent.isCurrent ? Theme.text : Theme.subText
                        font.pixelSize: 14
                        font.bold: parent.isCurrent
                        text: modelData.label
                    }
                    TapHandler { id: tap; onTapped: root.selected(modelData.key) }
                }
            }
        }
    }
}
