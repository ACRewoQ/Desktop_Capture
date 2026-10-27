import QtQuick

// Modal panel over the whole window: dimmed background (a tap outside closes it), title, the
// content (scrolls when it is taller than the window) and a close button.
Item {
    id: root
    property string title
    property bool shown: false
    property real panelWidth: 420
    property string closeLabel: "閉じる"
    property bool showClose: true
    default property alias content: body.data
    readonly property alias contentWidth: body.width

    signal closed()

    // While shown, what lies under the sheet is disabled: the siblings stacked below it and the
    // parent's own pointer handlers (Sheets.block). Taps on a sheet's button otherwise also reach
    // the button under it (mouse), and drags reach the faders and handlers under it (both).
    property var covered: null // while shown: what it disabled

    function cover() {
        const p = parent
        if (covered || !p) return
        const mine = p.children.indexOf(root)
        const list = []
        p.children.forEach((c, i) => { if (c !== root && (c.z < root.z || (c.z === root.z && i < mine))) list.push(c) })
        p.data.forEach(d => { if (d && d.grabPermissions !== undefined) list.push(d) }) // the parent's handlers
        list.forEach(t => Sheets.block(t))
        covered = list
        Sheets.openCount++
    }
    function uncover() {
        if (!covered) return
        covered.forEach(t => Sheets.unblock(t))
        covered = null
        Sheets.openCount--
    }
    onShownChanged: shown ? cover() : uncover()
    Component.onCompleted: if (shown) cover()
    Component.onDestruction: uncover() // a page unloaded with its sheet still open

    function close() {
        if (!shown) return
        shown = false
        closed()
    }

    anchors.fill: parent
    visible: shown
    z: 100

    Rectangle {
        anchors.fill: parent
        color: "#80000000"
        // A MouseArea (not a TapHandler) so that presses do not reach the page below.
        MouseArea { anchors.fill: parent; onClicked: root.close() }
    }

    Rectangle {
        id: panel
        anchors.centerIn: parent
        width: Math.min(root.panelWidth, root.width - 24)
        height: Math.min(header.height + body.implicitHeight + (root.showClose ? closeButton.height + 12 : 0) + 44,
                         root.height - 24)
        radius: 10
        color: Theme.panel
        border.color: Theme.border
        MouseArea { anchors.fill: parent } // keep taps on the panel from closing it

        Text {
            id: header
            anchors { top: parent.top; left: parent.left; right: parent.right; margins: 16 }
            horizontalAlignment: Text.AlignHCenter
            elide: Text.ElideRight
            color: Theme.text
            font.pixelSize: 18
            text: root.title
        }

        Flickable {
            id: scroller
            anchors {
                top: header.bottom; topMargin: 12
                bottom: root.showClose ? closeButton.top : parent.bottom; bottomMargin: 12
                left: parent.left; right: parent.right; leftMargin: 16; rightMargin: 16
            }
            clip: true
            contentHeight: body.implicitHeight
            interactive: contentHeight > height
            boundsBehavior: Flickable.StopAtBounds
            Column {
                id: body
                width: scroller.width
                spacing: 8
            }
        }

        MenuButton {
            id: closeButton
            visible: root.showClose
            anchors { bottom: parent.bottom; left: parent.left; right: parent.right; margins: 16 }
            label: root.closeLabel
            onTapped: root.close()
        }
    }
}
